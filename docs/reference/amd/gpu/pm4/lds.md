# Compute group memory

Local data share (LDS) is storage private to a workgroup and shared by its
workitems. The compiler describes fixed group requirements and generates LDS
accesses; the launch owner reserves enough storage for fixed and dynamic
regions. A workgroup barrier orders participating waves within that allocation.
A host cache flush or a later dispatch cannot substitute for that local
synchronization. [Group lifetime][llvm-lifetime] [GFX10/GFX11 LDS model][llvm-lds]

## Compiler requirement and launch allocation

An HSA descriptor's LDS register field is deliberately zero. Its fixed group
byte count is separate. Native AQL descriptor processing derives the allocation
from the dispatch packet's total group bytes; a raw PM4 launch owner realizes
the register field itself. This changes live launch state, not the linked
compiler image. [Descriptor contract][llvm-size]

For the GFX11 compute encoding, `COMPUTE_PGM_RSRC2.LDS_SIZE` occupies bits 23:15, in
512-byte units. PAL initializes an HSA pipeline from fixed group bytes and
can override it with the total dispatch allocation. The derived field is:

```text
units = ceil(total_group_bytes / 512)
RSRC2 = (resource_state & ~0x00ff8000) | (units << 15)
```

`resource_state` must already preserve the other compiler/runtime fields.
The total must satisfy the actual device's per-workgroup resource limit;
a nine-bit representation is not a statement of that limit. The cited PAL
device setup reports 64 KiB per workgroup. [Device limits][pal-granularity]
[Fixed-size realization][pal-size] [Dispatch override][pal-dynamic]

Encoding granularity differs from physical allocation. PAL states that
GFX10.3+ physically allocates 1024 bytes for a 512-byte request. Mesa rounds to
1024 bytes before encoding in 512-byte compute units. Thus PAL can encode 1
where Mesa encodes 2 for the same small requirement. These are distinct
runtime policies; neither field value measures occupied bytes or occupancy.
[PAL physical granularity][pal-granularity] [Mesa rounding][mesa-size]

PAL's RESOURCE_LIMITS policy sets SIMD_DEST_CNTL when the rounded wave count
per workgroup is divisible by four, with explicit overrides. That is a
scheduling policy, not part of the LDS byte count or an inter-dispatch fence.
[Wave policy][pal-limits]

## Dynamic regions and argument realization

A dynamic group pointer is a group-segment offset, not a global GPU address.
The compiler's fixed requirement excludes dynamic allocations. Each dynamic
region is aligned after preceding fixed/dynamic bytes, and the total launch
reservation covers every generated LDS access.

PAL's HSA argument realization demonstrates the two representations: its
input argument contains the requested dynamic byte count; the runtime aligns
the current group offset, places that offset into the GPU argument record and
increases the total group requirement. Supplying the byte count directly to a
shader that expects the realized offset would change the ABI.
[Argument realization][pal-arguments]

The executable owner retains the complete code/fetch extent. The argument
owner retains the realized offsets and all fetched argument bytes until the
last dispatch using them completes. Group memory itself has workgroup lifetime;
its initial contents and contents from a preceding dispatch are not input data.
Every value consumed from LDS needs an in-workgroup producer or another
explicitly defined initialization source. [Group lifetime][llvm-lifetime]

## Execution and synchronization

An ordinary cross-wave exchange follows this sequence:

```text
reserve the complete fixed + dynamic group extent
  → bind the realized LDS_SIZE and compatible workgroup/wave geometry
  → each workitem writes its owned LDS locations
  → complete relevant LDS instructions → workgroup barrier
  → workitems read the communicated LDS values → publish global results
```

For GFX10/GFX11, the workgroup is placed within one WGP; LDS is not a cached
global-memory allocation. Compiler wait instructions and a uniform workgroup
barrier supply the cross-wave ordering. Global result publication still needs
the separate shader-completion and memory-visibility protocol described in
[compiled dispatch](dispatch.md#publication-and-shader-dependencies).
[LDS synchronization model][llvm-lds]

## Rebinding resource sizes

PAL separates the selected pipeline's resource state from an override retained
within its current binding. `GfxCmdBuffer::CmdBindPipeline` replaces the
dynamic compute settings with the new bind request. In the older register
path, `PipelineChunkCs::WriteShCommands` starts from that pipeline's immutable
register state, applies the current overrides and emits `COMPUTE_PGM_RSRC2`
and `COMPUTE_RESOURCE_LIMITS`. GFX12 emits its selected register array directly,
or a local copy with dynamic overrides. A new binding with no LDS override
therefore restores that program's fixed request, including a smaller request;
it does not inherit the preceding program's largest LDS setting.
[Bind-state owner][pal-bind-state] [Older binding][pal-full-bind]
[Older resource writes][pal-resource-writes] [GFX12 binding][pal12-full-bind]

While the same HSA binding remains active, PAL's automatic argument-realization
path grows its retained override when the computed LDS requirement exceeds
the bound size. Both the older and GFX12 paths make that comparison. This is
distinct from selecting a new pipeline and its bind settings. Mesa's
variable-size path re-emits resource state when the program or requested
shared size changes, including smaller requests. These are runtime policies
over the launch-state representation. [PAL retained override][pal-rebind]
[GFX12 retained override][pal12-rebind] [Mesa rebinding][mesa-rebind]

With [conditional command flow](conditional.md#structured-control-flow-and-ownership),
each selected path establishes the resource state consumed by its dispatches.
A join cannot infer live state from the last arm recorded by the CPU: that
arm might never execute. PAL disables its register-write optimizer when
recording conditionals or loops because the optimizer does not model those
paths. [Conditional state][pal-control-flow] [Loop state][pal-loop-control-flow]

Resource binding and payload synchronization remain separate. The cited PAL
bind writers emit register and optional prefetch commands without inserting
a general shader drain. Dependencies supply the completion and visibility
needed by a successor's reads; a changed LDS field is neither such a dependency
nor a way to resize already executing workgroups. The successor's LDS has its
own workgroup lifetime. [Binding][pal-full-bind] [GFX12 binding][pal12-full-bind]
[Group lifetime][llvm-lifetime]

A completed-use CPU rebuild may change the arguments and allocation request
only after prior users have finished. A GPU command stream may bind distinct
immutable programs and resource requests with the dependencies required by its
payload graph. In either case, retaining more physical LDS than a later launch
needs could produce the same output as an exact smaller allocation. Register
programming, correctness of addressed bytes, physical reclamation and occupancy
are different properties.

[llvm-lifetime]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L1181-L1189
[llvm-lds]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L13438-L13464
[llvm-size]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L6665-L6688
[pal-size]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputePipeline.cpp#L63-L82
[pal-dynamic]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L838-L856
[pal-granularity]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L5622-L5641
[mesa-size]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_shader_util.h#L352-L365
[pal-limits]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L301-L345
[pal-arguments]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L2457-L2493
[pal-rebind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L772-L807
[pal-bind-state]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L1407-L1422
[pal-full-bind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L680-L750
[pal-resource-writes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineChunkCs.cpp#L770-L833
[pal12-full-bind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineChunkCs.cpp#L862-L911
[pal12-rebind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1272-L1291
[pal-control-flow]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdStream.cpp#L251-L291
[pal-loop-control-flow]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdStream.cpp#L344-L361
[mesa-rebind]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_compute.c#L293-L325
