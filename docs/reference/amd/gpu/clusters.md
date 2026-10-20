# Workgroup clusters and multicast loads

CDNA5 workgroup clusters place cooperating workgroups on one shader engine.
They provide a workgroup-counted barrier and combine matching loads so that
one memory response can serve several workgroups. Each participating workgroup
still owns its request, completion observation and LDS storage. Combining
requests saves memory traffic; it does not create one shared LDS allocation
or a single completion that retires every recipient's readers.
[Cluster organization, §2.3][isa-clusters] · [Multicast loads, §10.7][isa-loads]

## Placement and identity

The 27 July 2026 CDNA5 ISA describes one-, two- or three-dimensional clusters
of up to **16 workgroups**. Every workgroup in a cluster runs on a different
WGP within the same shader engine. A WGP can concurrently host workgroups
from different clusters. Workgroup size is uniform within a cluster, and
cluster size is uniform within a dispatch. A size-one group does not enable
cluster behavior. [Placement and size][isa-clusters]

Logical identity and physical placement have different lifetimes:

| State | Representation and lifetime |
| --- | --- |
| `WG_in_Cluster` | Four-bit logical workgroup index in `IB_STS2`, from zero through cluster size minus one. Preserved across context switches. |
| `Cluster_ID` | Four-bit physical identity in `IB_STS2`; zero means no cluster and 1–15 identify active physical clusters. Can change across context switches. |
| `flat_NWG` | `TTMP6[27:24]`, total workgroups in the cluster minus one. |
| `nwg_x`, `nwg_y`, `nwg_z` | `TTMP6[15:12]`, `[19:16]`, `[23:20]`, workgroups in each dimension minus one. |
| `wg_x`, `wg_y`, `wg_z` | `TTMP6[3:0]`, `[7:4]`, `[11:8]`, logical workgroup coordinates within the cluster. |

The multicast mask indexes logical workgroups. Physical cluster IDs and WGP
placement consequently cannot serve as persistent application identities.
The manual explicitly permits different physical CUs after context restore.
[Cluster state, §2.3.1][isa-state] · [Context restore, §5.6.6.1][isa-restore]

## Native launch geometry

AMD's `hsa_amd_ext_kernel_dispatch_packet_t` uses the vendor-specific AQL
packet type and `HSA_AMD_PACKET_TYPE_EXT_KERNEL_DISPATCH = 3`. Its geometry
differs from the [standard dispatch packet](aql/dispatch.md#packet-and-executable-representation):

| Extended packet byte offset | Field | Unit and width |
| --- | --- | --- |
| 2 | `amd_format` | Eight-bit vendor packet selector. |
| 3 | `setup` | Eight-bit dispatch setup. |
| 4, 6, 8 | `workgroup_size_x/y/z` | Workitems per workgroup; 16 bits per axis. |
| 12 | `cluster_count_x` | Clusters in the grid's X dimension; 32 bits. |
| 16, 18 | `cluster_count_y/z` | Clusters in Y/Z; 16 bits per axis. |
| 20, 21, 22 | `cluster_size_x/y/z` | Workgroups per cluster; eight bits per axis. |

The dimensions are positive; unused axes have size and count one. An
eight-bit field is a representation limit, not an admission guarantee.
`HSA_AMD_AGENT_INFO_CLUSTER_MAX_DIM` and `CLUSTER_MAX_SIZE` describe workgroups
within a cluster, while `KERNEL_CLUSTER_MAX_DIM` and `KERNEL_CLUSTER_MAX_SIZE`
describe the grid of clusters. [Vendor selector][hsa-type] · [Packet fields][hsa-packet]
· [Distinct capability queries][hsa-cluster-info]

ROCr enables its extended-dispatch and cluster paths when the selected ISA's
major version is 12 and minor version is at least 5. Its cluster dimension
limits come from `NumArrays * NumCUPerArray`; the total-size query returns
that value once, not its cube. The pinned CLR consumer separately replaces a
reported size greater than one with `maxComputeUnits_ / numberOfShaderEngines_`.
Those are source-specific topology policies, separate from the manual's
16-workgroup architectural ceiling. [ROCr selection][rocr-selection]
[ROCr query][rocr-query] [CLR capability consumer][clr-capability]

For the public clustered GEMM caller, Triton launches physical clusters only
along X: cluster dimensions `(num_ctas, 1, 1)` and workgroup-grid dimensions
`(gridX * num_ctas, gridY, gridZ)`. The logical matrix decomposition can still
be 4×4. The compiler obtains the logical workgroup index from the cluster's
X coordinate and makes `program_id` identify a cluster instead of a workgroup.
[Launch builder][triton-launch] · [Workgroup identity][triton-workgroup-id]
· [Program identity][triton-program-id]

The launch builder calls `hipDrvLaunchKernelEx` and requires that symbol to
exist. Its attribute ID 4 is `hipLaunchAttributeClusterDimension`. HIP carries
the dimensions through `HIPLaunchParams` and `NDRangeContainer`; CLR selects
the extended packet and computes each cluster count as total workitems divided
by workgroup size and cluster size. The HIP geometry owner requires integral
cluster tiling. [Attribute ABI][hip-attribute] · [Driver launch][hip-launch]
· [Geometry owner][clr-geometry] · [Extended packet builder][clr-packet]

Cluster launch and [cooperative-grid execution](cooperative.md) have separate
selection paths. In the pinned HIP driver entry point, a nonzero cooperative
attribute returns through the cooperative launch branch before constructing
the launch parameters that carry the supplied cluster dimensions. The
presence of both attributes therefore does not establish their composition;
kernel metadata can independently supply required cluster dimensions.
[Attribute dispatch][hip-launch] · [Kernel metadata owner][hip-metadata]

## Request matching and masks

Each selected workgroup contributes one requesting wave for a matching load.
Requests combine when all selected workgroups have requested, or when the
combining timeout expires. At timeout, the response serves the waves that
have already requested. Later requestors receive a separate response. Thus
a delayed workgroup can reduce combining without losing its own request.
[Request and timeout semantics][isa-loads]

For `CLUSTER_LOAD_B{32,64,128}` and
`CLUSTER_LOAD_ASYNC_TO_LDS_B{32,64,128}`, M0 supplies these fields:

| M0 bits | Meaning |
| --- | --- |
| 15:0 | One bit per logical workgroup. Matching requestors use the same mask. |
| 16 | Early timeout: return data as soon as the cache supplies it to the requestors already present. Zero selects the normal timeout. |

A zero workgroup mask returns data only to the requesting WGP; it does not
suppress the load. Different M0 values can prevent requests from combining.
These instructions support the global `GV` and `GVS` address forms and force
a WGP-cache miss regardless of the scope and temporal fields. Outside a
cluster they become ordinary global loads. [Mask, addressing and cache
behavior][isa-load-completion]

For tensor loads, descriptor group 1, DWORD 0 bits 15:0 carry
`D#.workgroup_mask`, and bit 21 carries `D#.early_timeout`. The latter is
appended to the mask when the TDM sends requests to the cache and requests
immediate return to the requestors present when the data arrives. These are
descriptor fields; an explicit cluster-load instruction supplies the mask
and timeout through M0 instead. A nonzero descriptor mask makes
`TENSOR_LOAD_TO_LDS` use cluster asynchronous loads internally. Tensor stores
ignore the mask, and a wave outside a cluster supplies zero.
[Tensor selection, §10.11.3][isa-tensor-mask] · [Descriptor field table][isa-descriptor]

Triton's multicast predicate accepts its GFX1250 family only without the
`-strict` variant. Multi-workgroup launch and TDM support have their own
predicates and remain enabled for that variant. ROCr selects a registered
strict ISA for revision-zero silicon under its strict-target policy; a
cluster-capable target therefore does not imply multicast lowering is enabled.
[Compiler predicates][triton-features] · [Strict-target selection][rocr-selection]

The compiler's `getMaxMulticastMaskPopcount()` returns **5** when multicast is
enabled. Its layout-based mask builder keeps at most `floor(log2(5)) = 2`
free workgroup-index bits, producing power-of-two communication groups of
at most **4**. Extra free bits become subgroup selectors. For example, a
sharing group with free index bits 0, 2 and 3 splits into `{0,1,4,5}` and
`{8,9,12,13}`. Every selected subgroup contains its requestor. This records
the compiler's explicit limit and algorithm; the 16-bit ISA mask alone does
not establish a larger supported population. [Population limit][triton-features]
· [Mask construction][triton-mask]

The change introducing that split attributes the five-member bound to
`gfx1250` hardware: masks with more members lose multicast combining. The
compiler emits a performance remark and divides the group to retain the
benefit. This rationale concerns combining, not a five-workgroup limit on
cluster launch. It also does not explain the separate strict-target
exclusion. [Population-limit rationale][triton-mask-rationale]

## Completion and storage ownership

Completion belongs to the requesting wave in each recipient workgroup:

| Operation | Observation | What becomes ready |
| --- | --- | --- |
| `CLUSTER_LOAD_B{32,64,128}` | That wave's `LOADcnt`. | The requesting wave's destination VGPRs. |
| `CLUSTER_LOAD_ASYNC_TO_LDS_B{32,64,128}` | That wave's `ASYNCcnt`. | The transfer into that workgroup's LDS. |
| Multicast `TENSOR_LOAD_TO_LDS` | Issuing wave's `TENSORcnt`, or its configured descriptor notification. | The tensor transfer covered by that observation. |

[Cluster counters and ordering][isa-load-completion] · [Tensor completion
and descriptor notifications](async-memory.md#transfer-families-and-completion-units)

Other waves in a recipient workgroup require a barrier or memory-atomic
handoff from its requestor before consuming LDS. The counter in one workgroup
does not observe another workgroup's local readers. Cluster loads retain
completion ordering with loads using the same counter class, not with every
instruction or transfer class. [Recipient notification][isa-load-completion]

This gives three separate ownership edges:

| Storage | Reuse boundary |
| --- | --- |
| Shared global source | Every transfer that can still read it has completed, including late requestors served separately. |
| One recipient's LDS slot | Its fill has completed and all local readers have retired. Other workgroups' disjoint LDS slots retain their own boundaries. |
| Global output | All producing transfers complete, followed by the release/acquire operations required by the next observer. |

The first row follows from the timeout semantics: the first broadcast can
finish before a later request has even begun. The second follows from the
per-requestor completion domain. Neither combining nor a mask is an
acknowledgment from all consumers. The [local ready/empty protocol](async-memory.md#reusable-input-and-output-slots)
and [global publication rules](shader-memory.md#global-release-and-acquire-sequences)
remain separate parts of a composed pipeline.

## Cluster barrier participants

`S_BARRIER_SIGNAL -3` and `S_BARRIER_WAIT -3` address the user cluster barrier.
Its member count starts at the number of workgroups, and its signal count
starts at zero. **One wave per workgroup signals; all waves wait.** To make
an arrival represent work performed by every wave, the workgroup first joins
those waves with the required memory ordering. The cluster barrier itself
counts workgroups, not their threads or outstanding transfers.
[Cluster barrier, §5.6.6][isa-barrier]

The signal count resets when the barrier completes. Workgroup termination
decrements the member count; ordinary cluster signal instructions cannot
set that count. Outside a cluster these barrier operations are no-ops.
`S_BARRIER_SIGNAL_ISFIRST` sets SCC to one for the first arrival and otherwise
leaves SCC unchanged, so the election starts with SCC zero. The distinct
cluster-trap barrier uses ID -4. [Barrier operations][isa-barrier]
· [Membership and restore][isa-restore]

Triton's cluster-arrive conversion selects wave zero to issue signal -3;
cluster-wait emits wait -3 for every participating wave. Arrive does not
insert a workgroup memory barrier. A caller that uses cluster completion to
retire a shared global source must first account for all relevant local
transfers, then signal once per workgroup and wait across the cluster.
An external observer still needs its own release/acquire protocol; cluster
completion alone is not a global cache-publication operation.
[Signal/wait lowering][triton-cluster-barrier]

The manual's state table 23 gives five-bit member and signal counts, while
the same page's context-save message expression shows seven-bit count slices.
These are distinct public descriptions, not an established packed-state ABI.
The ordinary signal/wait protocol above does not depend on synthesizing that
context-save representation. [State table and message expression][isa-barrier]

## Two-slot clustered GEMM flow

The public BF16 GEMM caller uses 16 workgroups arranged as a logical 4×4
matrix tile, eight waves per workgroup and two LDS slots for each input. Its
matrix layouts determine which workgroups share A and B slices. The host
launches `num_ctas=16`; this is a concrete compiler caller, rather than a
rule that every clustered program needs that shape.
[Caller layouts and launch][triton-gemm-launch]

The input loop has the following ownership sequence:

1. Issue the first A/B fills into slot zero. Descriptor advancement selects
   the next K block without changing the lifetime of the initial transfer.
2. Wait for the required tensor transfers before reading the current slot.
   Issue the next A/B fills into the alternate slot while the current slot
   supplies operands to computation.
3. Retire the current slot's local reads before that slot becomes a refill
   destination. The compiler's local-memory dependency analysis and warp
   pipeline boundaries supply this edge.
4. Alternate slots through the K blocks. Cluster arrive/wait keeps workgroups
   closer in progress and can improve request combining; the caller assigns
   readiness and LDS reuse to the tensor waits and local pipeline boundaries.
5. Drain the last input fill, finish its reads, stage the result in output
   LDS, issue the tensor store and finish with `tdm.async_wait(0)` before
   that output storage retires.

[Input loop and tail][triton-gemm-loop] · [Consume/refill helper][triton-gemm-refill]
· [Output store and drain][triton-gemm-store]

The compiler's warp-pipeline implementation also calls its *instruction
stages* “clusters.” Those stages are within one workgroup and are distinct
from hardware workgroup clusters. It partitions the eight waves into two
four-wave groups, analyzes overlapping LDS read/write effects, and places
local barriers across the relevant stage paths, including loop backedges.
Local barriers lower to workgroup-scoped release/acquire fences around a
workgroup execution barrier; the memory legalizer selects the target waits.
[Dependency analysis][triton-pipeline-dependencies]
[Wave partition][triton-pipeline-partition] [Local-barrier lowering][triton-local-barrier]

An async-ready dependency suppresses the already-satisfied read-after-write
edge, but the analysis keeps the reverse read-before-refill edge. Where the
pipeline reuses a pre-existing async wait as a stage boundary, its lowering
explicitly relies on the producer having no unresolved local-fence requirement
there. A tensor wait by itself is not evidence that every local reader has
finished. [Directional dependency filter][triton-reuse-filter]
· [Existing-wait boundary contract][triton-pipeline-existing-wait]

[isa-clusters]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=19
[isa-state]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=20
[isa-barrier]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=60
[isa-restore]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=61
[isa-loads]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=143
[isa-load-completion]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=144
[isa-tensor-mask]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=151
[isa-descriptor]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=153
[hsa-type]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L130-L137
[hsa-packet]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L298-L385
[hsa-cluster-info]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L953-L972
[rocr-selection]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L233-L260
[rocr-query]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2710-L2723
[clr-capability]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L1837-L1855
[triton-launch]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/backend/driver.c#L641-L683
[triton-workgroup-id]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/TargetInfo.cpp#L157-L170
[triton-program-id]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/Utility.cpp#L348-L376
[hip-attribute]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/hip/include/hip/hip_runtime_api.h#L1753-L1802
[hip-launch]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_module.cpp#L1420-L1501
[hip-metadata]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_module.cpp#L396-L438
[clr-geometry]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/platform/ndrange.hpp#L98-L193
[clr-packet]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L5004-L5029
[triton-features]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/Dialect/TritonAMDGPU/IR/TargetFeatures.cpp#L254-L270
[triton-mask]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/Utility.cpp#L403-L473
[triton-mask-rationale]: https://github.com/triton-lang/triton/commit/71d3f5cf482a3954bf1294d2770aef35d0fa6cd4
[triton-cluster-barrier]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/BarrierOpToLLVM.cpp#L119-L164
[triton-gemm-launch]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/python/examples/gluon/gemm_warp_pipeline_cdna5.py#L1172-L1199
[triton-gemm-loop]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/python/examples/gluon/gemm_warp_pipeline_cdna5.py#L193-L246
[triton-gemm-refill]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/python/examples/gluon/gemm_warp_pipeline_cdna5.py#L156-L176
[triton-gemm-store]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/python/examples/gluon/gemm_warp_pipeline_cdna5.py#L113-L123
[triton-pipeline-dependencies]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/ConvertWarpPipeline.cpp#L164-L268
[triton-pipeline-partition]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/ConvertWarpPipeline.cpp#L1044-L1048
[triton-local-barrier]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/MemoryOpToLLVM.cpp#L620-L667
[triton-reuse-filter]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/MembarUtility.cpp#L61-L77
[triton-pipeline-existing-wait]: https://github.com/triton-lang/triton/blob/8262c9a91a1d6828ad4f36437fa0046daa67720d/third_party/amd/lib/TritonAMDGPUToLLVM/ConvertWarpPipeline.cpp#L636-L648
