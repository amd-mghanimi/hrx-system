# PM4 compute commands

PM4 is the command processor's packet language. A compute stream binds shader
state, launches work, moves data, and establishes dependencies between execution
stages and memory clients. The stream's transport also owns its address space,
queue context, publication protocol, and command-storage lifetime. Those facts
are not encoded by an opcode alone.

This section follows ordinary MEC compute callers in PAL, Mesa and Linux/KFD.
ME and PFP forms are identified separately where the packet or ordering rules
differ. PAL's `gfx9` source directory contains definitions for later generations;
the directory name is not an architecture predicate. [Packet definitions][packets]
[Generation predicates][generations]

## Programming sequence

A complete compute operation has several owners:

1. The native queue owner establishes the process VM, scheduling context and
   runtime-controlled shader state.
2. The program owner supplies a compatible executable, its complete readable
   fetch extent, resource requirements and initial-register ABI.
3. The submission owner makes command bytes, arguments and input data visible
   before publishing the queue or indirect-buffer reference.
4. The command processor binds the program and launches work. Dependencies join
   the actual producer stage before applying the required cache operations.
5. A completion protocol publishes a result to its intended observer. Command
   consumption and the last dependent consumer separately determine which
   storage can be reused.

PAL's compute postamble illustrates why the distinctions matter: it drains
outstanding CP DMA and, when command-storage busy tracking is enabled, waits
for shaders that may access command memory before incrementing the tracker.
That tracker relies on a subsequent KMD cache-flushing EOP in PAL's scheduled
submission path. With tracking disabled, the client owns completion before
returning storage. A raw user ring has no implicit right to PAL's native
trailer. [Compute postamble][postamble]
[Allocator modes and reuse](command-buffers.md#cpu-rebuild-after-completed-use)

## Topics

| Chapter | Native mechanism |
| --- | --- |
| [Queue publication](publication.md) | Ring capacity, DWORD frontiers, host visibility, doorbells and the distinct KFD, scheduled DRM and DRM userq owners. |
| [Memory commands](memory-commands.md) | Memory-operation overview, packet framing, waits and shader-completion firmware predicates. |
| [Control-value copies](copy.md) | COPY_DATA fields, selectors, MEC/ME/PFP differences, GFX12/GC12.1 controls, sampling and result ownership. |
| [Inline writes](write.md) | WRITE_DATA fields, source-specific routing, captured input, small updates, resident descriptors, control-cell reuse and later command readers. |
| [Register and memory waits](wait.md) | WAIT_REG_MEM and WAIT_REG_MEM64 fields, engine operation differences, offload, native producers, cache dependencies and control-cell reuse. |
| [Completion publication](release.md) | RELEASE_MEM fields, confirmation and interrupt selectors, legacy EVENT_WRITE_EOP/EOS, event/clock/value owners and storage retirement. |
| [Atomic operations](atomics.md) | TC integer operations, returned values, command modes, participant domains and cache/retirement contracts. |
| [Cache control](cache.md) | Acquire/release fields, native-generation differences, ranges, scopes, recorded BLT/cache history and complete visibility sequences. |
| [Address translation](translation.md) | PRIME_UTCL2 warmup and INVALIDATE_TLBS fields; actual shader-prefetch callers, page-table publication, KIQ/MES selection, acknowledgments and mapping retirement. |
| [Compiled dispatch](dispatch.md) | Executable backing, register and argument ABI, DISPATCH_DIRECT and initiator field families, offset/partial-group geometry, GFX12 distribution controls, MEM_ORDERED mode, runtime state and completion. |
| [Shader register transport](registers.md) | SET_SH_REG, SET_SH_REG_INDEX, PAIRS/PACKED forms, LOAD_SH_REG and LOAD_SH_REG_INDEX; engine and firmware selection, field units, inline copies, borrowed inputs and context lifetime. |
| [Compute affinity and queue priority](../scheduling.md) | `COMPUTE_STATIC_THREAD_MGMT_SE*`, KMD mask composition, per-SE preambles and the distinct KFD queue controls. |
| [Group memory](lds.md) | Static and dynamic LDS allocation, workgroup synchronization and resource rebinding. |
| [Indirect dispatch](indirect.md) | DISPATCH_INDIRECT engine forms, memory-resident workgroup/workitem dimensions, compiler inputs, interleaved packet views and producer-to-fetch dependencies. |
| [Execute-indirect commands](execute-indirect.md) | EXECUTE_INDIRECT_V2 MEC/PFP fields, argument/count records, packed copy metadata, register runs, dispatch tails, firmware selection and embedded versus queue-global spill ownership. |
| [Conditional execution](conditional.md) | COND_EXEC ranges, COND_INDIRECT_BUFFER branches and [reference masking](conditional.md#reference-masking-and-portability), Boolean sampling and reuse, and PRED_EXEC virtual-XCC selection. |
| [Command buffers](command-buffers.md) | INDIRECT_BUFFER engine fields, PASID/constant forms, CHAIN continuations, REWIND, generated-command publication, WDDM native retirement and completed-use rebuild. |
| [Cross-queue handoff](handoff.md) | Release, control signaling, wait, consumer acquire and last-use ownership. |
| [Command-processor DMA](dma.md) | CP_DMA, DMA_DATA and DMA_DATA_FILL_MULTI fields; engine and count revisions, actual copy/fill/prefetch selection, completion, cache routing and source/destination lifetimes. |
| [Timing](timing.md) | Sampling stage, timestamp visibility, clock domains and profiling ownership. |
| [Pipeline-statistics queries](pipeline-statistics.md) | SAMPLE_PIPELINESTAT EVENT_WRITE, PIPELINESTAT_START/STOP, compute enable policy, counter records, availability, resolves and final storage users. |
| [Performance counters](counters.md) | Event and instance selection, register fields, sample widths, collection sequencing and completed-use result ownership. |
| [Performance queries](counter-queries.md) | RADV/Vulkan profiling locks, private submission serialization, counter-pass layout, result decoding and native clock-owner lifetime. |

[Architecture identity](../architectures.md) distinguishes compiler targets,
native IP versions and firmware. [AQL](../aql/README.md) and
[SDMA](../sdma/README.md) are different packet/transport contracts; sharing a
memory allocation does not make their completion or cache operations
interchangeable. [Programming recipes](../recipes/README.md) compose those
contracts across actors.

## Interpreting the source

A packet definition establishes representation. A builder selects fields. A
caller establishes the execution sequence, and the allocation/submission owner
establishes publication and retirement. The chapters keep those layers visible.
They also retain disagreements: MEC and graphics ACQUIRE_MEM range widths,
PAL and Mesa DMA completion conventions, and metadata-cache policies cannot be
resolved merely by choosing a familiar packet name.

Execution completion, write confirmation, cache visibility, notification and
command retirement answer different questions. In particular, an instruction
cache invalidate does not retire an older shader, a memory wait does not by
itself acquire payload caches, and a queue read pointer is not a shader result.
The applicable generation, engine, firmware and mapping predicates belong to
each complete sequence. [Compute waits][idle] [Cache-only acquire][acquire]

[packets]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L43-L136
[generations]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L2328-L2333
[postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1230-L1267
[idle]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4284-L4337
[acquire]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L398-427
