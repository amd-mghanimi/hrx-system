# Indirect compute dispatch

DISPATCH_INDIRECT reads launch dimensions from GPU memory while using the
currently bound compute program and resource state. The count tuple is a CP
input. Any grid-size values consumed by the shader are a separate compiler ABI
obligation. The producer-to-fetch dependency and the tuple's last consumer
must both be known before its storage can be changed.

[EXECUTE_INDIRECT_V2](execute-indirect.md) is a separate mechanism: it consumes
a sequence of records with user-data updates and a dispatch tail. Its count,
packed metadata and queue-global spill state have additional owners.

## Packet and count representation

The ordinary MEC form is opcode `0x16`, four DWORDs: type-3 header, absolute
address low/high and DISPATCH_INITIATOR. PAL requires four-byte alignment and
uses three consecutive unsigned 32-bit X/Y/Z workgroup counts. A zero dimension
discards the dispatch in its public contract. The ME form is three DWORDs and
uses a 32-bit byte offset relative to an address established by SET_BASE; it is
not the same address operand. [MEC builder][pal-mec] [ME builder][pal-me]
[Argument layout][pal-args] [Caller contract][pal-api]

| Word | MEC, four DWORDs | ME/PFP graphics form, three DWORDs |
| --- | --- | --- |
| 0 | Type 3 in bits 31:30, count 2 in bits 29:16, opcode `0x16` in bits 15:8. | Type 3, count 1, opcode `0x16`. |
| 1 | `addr_lo`, all 32 bits of the low address; the caller enforces alignment. | `data_offset`, 32-bit byte offset. |
| 2 | `addr_hi`, all 32 bits of the high address. | `dispatch_initiator`. |
| 3 | `dispatch_initiator`. | Absent. |

[MEC fields][pal-mec-fields] [Graphics fields][pal-gfx-fields]
The [dispatch-control family](dispatch.md#initiator-field-family) supplies
the initiator's source-specific bit meanings. The representation's address
width does not establish which virtual addresses the native mapping accepts.

PAL's ordinary MEC builder leaves USE_THREAD_DIMENSIONS clear and sets
COMPUTE_SHADER_EN, FORCE_START_AT_000 and ORDER_MODE, plus the program's wave32
and selected tunneling/preemption controls. NUM_THREAD_X/Y/Z still describe
the local group. Thus a tuple `(16,1,1)` with local `(64,1,1)` requests 1024
workitems, not sixteen. RADV also has an explicitly `unaligned` internal path
that sets USE_THREAD_DIMENSIONS and programs full local dimensions; that path
must not be conflated with the ordinary group-count API.
[PAL initiator][pal-mec] [RADV dispatch construction][mesa-emit]

The pinned producers use different low header bits: PAL's type-3 defaults yield
`0xc0021600`; RADV ORs the compute shader bit and emits `0xc0021602`. PAL's MEC
header names the low byte reserved, whereas its shared header builder supports
ME/PFP shader-type fields. These actual conventions are recorded separately;
one does not establish that every low-bit permutation is interchangeable.
[Header definition][pal-header] [PAL defaults][pal-defaults]
[RADV emitter][mesa-emit]

RADV selects the absolute-address form specifically when the command buffer
uses its compute queue and `gfx_level >= GFX7`. Otherwise its emitter supplies
SET_BASE index 1 and the three-DWORD relative form. RadeonSI's cited shared
emitter supplies SET_BASE and the relative form; its queue naming alone is
not a replacement for checking that actual path. PAL's GFX12 graphics caller
puts the high address bits in SET_BASE and passes the low 32 bits as offset;
its compute caller needs no SET_BASE for the absolute-address form.
[RADV engine predicate][mesa-mec-selection] [RadeonSI emitter][mesa-si-indirect]
[PAL base ownership][pal12-base]

RADV sets ORDER_MODE for its selected GFX7+ devices and clears it for ordered
dispatches. Compute-only GFX940+ takes a different default from graphics-capable
devices. Tunneling is another separately selected policy. ORDER_MODE supplies
neither execution completion nor payload visibility.
[Default policy][mesa-policy] [Per-dispatch policy][mesa-emit]

## Shader-visible inputs

PAL's PAL-ABI path checks `numWorkGroupsRegAddr`. If present, it writes the
indirect tuple's address to the ABI-selected user SGPRs; a direct launch instead
materializes its logical dimensions in embedded memory. RADV checks
`AC_UD_CS_GRID_SIZE` and loads all three values into user SGPRs with
LOAD_SH_REG_INDEX on GFX10.3+, otherwise passing their pointer. The CP's
indirect fetch does not perform these ABI operations implicitly. RADV records
hangs on older compute queues even though LOAD_SH_REG_INDEX exists on GFX8+;
its predicate retains that compute-engine restriction.
[PAL user data][pal-abi] [RADV grid input][mesa-grid]
[SGPR policy][mesa-grid-policy]

The [register-transport chapter](registers.md#indexed-load_sh_reg_index)
describes the load's address modes, data formats and source-storage lifetime.
Its direct-address grid-input flow is distinct from PFP offset-mode loads
used by execute-indirect command generation.

Other compiler inputs can include dispatch pointers, hidden grid sizes,
preloaded arguments or scratch state. They must match the actual entry ABI.
PAL's public indirect-dispatch API expressly excludes HSA-ABI pipelines;
that is an implementation boundary of this caller, not a hardware prohibition
on all manually prepared HSA entries. [PAL API boundary][pal-api]
[Pipeline selection][pal-selection] [Compiler input rules][llvm-inputs]

## Publication and last use

The tuple is borrowed storage, not a copied packet operand or an atomic
12-byte snapshot. It remains stable until every CP fetch and shader reader
using it completes. A scalar load through an ABI-provided pointer may extend
its lifetime beyond command parsing. Code, arguments and payload allocations
have their own last-use obligations.

PAL's GFX10/GFX11 planner classifies indirect arguments as GL2 clients. A CPU
producer bypasses GL2 in that model, so publication toward a GL2 consumer
requests GL2 writeback/invalidation. Its GFX12 planner instead classifies
indirect-argument fetches as bypassing GL2; a shader producer's GL2 data must
be written back for that CP reader. A shader that also reads the tuple can
require its scalar/vector cache acquire in addition to the CP fetch edge.
[Access classes][pal-cache] [CPU-to-GL2 mapping][pal-cpu]
[Indirect acquire stage][pal-indirect-acquire]
[GFX12 client and transition rules](cache.md#gfx12-cp-and-shader-handoffs)

For GPU-produced counts, the ordinary sequence is:

```text
producer shader writes the complete valid tuple
  → join producer execution → perform producer/consumer cache operations
  → bind the consumer's complete ABI → DISPATCH_INDIRECT
  → complete the consumer and its output publication
  → retire every remaining tuple, argument and command-storage reference
```

PAL's barrier lowering waits for compute execution before trailing cache work.
The compute acquire point is ME; there is no PFP dependency to borrow. A plain
ACQUIRE_MEM cannot replace the producer join, and observing the ring read
pointer cannot replace the final consumer's completion.
[Execution before cache work][pal-cs-publish]
[Indirect acquire placement][pal-indirect-acquire]

RADV's indirect-copy meta operation supplies a concrete workitem-count
producer: its preprocessing shader writes a three-DWORD invocation tuple;
the caller requests compute-stage completion and buffer cache maintenance
before consuming it with `unaligned=true`. The tuple's producer and selected
dispatch mode agree on units. Replacing that producer with workgroup counts
without changing the consumer would change the launched work.
[Tuple producer][mesa-meta-counts] [Dependency and consumer][mesa-meta-dispatch]

The tuple's count units do not bound shader data accesses by themselves.
Backing must cover the work launched by the selected dimensions and the
program's actual argument-dependent footprint. A shader's internal bound may
reduce payload accesses without changing the hardware workgroup count.

## Generation-specific differences

RADV's asynchronous-compute alignment repair is specifically for its GFX7
predicate. If a tuple is not 32-byte aligned, it allocates aligned upload
storage and copies the three words with confirmed COPY_DATA before dispatch.
That workaround does not change the ordinary four-byte packet/API alignment
on later generations. [Bug predicate][mesa-align] [Actual repair][mesa-emit]

PAL's GFX12 MEC builder retains a four-DWORD absolute-address packet. This
representation continuity does not make GFX12 resource registers, cache
operations or preemption policy identical to GFX10/GFX11.
[GFX12 builder][pal-gfx12] [Direct binding](dispatch.md)

### Interleaved indirect packet views

GFX12 `DISPATCH_INDIRECT_INTERLEAVED`, opcode `0xa8`, also has different
PFP and ME definitions:

| View | Header and payload words |
| --- | --- |
| PFP, three DWORDs | Word 0 type-3/count 1; word 1 32-bit `data_offset`; word 2 initiator. |
| ME, seven DWORDs | Word 0 type-3/count 5; word 1 `dim_z` in bits 15:0 with bits 31:16 reserved; words 2–3 32-bit `prescale_dim_x/y`; word 4 32-bit `dim_x`; word 5 `dim_y` in bits 15:0 with bits 31:16 reserved; word 6 initiator. |

[PFP definition][pal12-pfp-interleaved] [ME definition][pal12-me-interleaved]
[Opcode][pal12-opcodes]

PAL's graphics builder writes the PFP offset form and chooses opcode `0xa8`
when 2D dispatch interleave is selected. Its MEC builder continues to use
ordinary `0x16`; its compute caller disables 2D dispatch interleave. The
seven-DWORD ME definition therefore does not describe the bytes authored by
either ordinary caller, and its prescale semantics cannot be inferred from
the pointer-based API. The [distribution policy](dispatch.md#gfx12-distribution-controls)
describes the actual selection, register setup and constraints.
[Graphics builder][pal12-interleaved-builder] [MEC builder][pal-gfx12]

[pal-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1538-L1572
[pal-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1275-L1301
[pal-args]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L1404-L1410
[pal-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L3051-L3070
[mesa-emit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15035-L15149
[pal-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L43-L54
[pal-defaults]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L805-L810
[mesa-policy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device.c#L1640-L1653
[pal-abi]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L719-L734
[mesa-grid]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15083-L15099
[mesa-grid-policy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L2796-L2797
[pal-selection]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L160-L167
[llvm-inputs]: https://github.com/llvm/llvm-project/blob/6dfe1677ab8dffbc6ec13d53a1e0215d75147689/llvm/docs/AMDGPUUsage.rst#L6920-L7004
[pal-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L56-L68
[pal-cpu]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L352-L364
[pal-indirect-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L534-L570
[pal-cs-publish]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L2027-L2042
[mesa-align]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1021-L1037
[pal-gfx12]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1328-L1360
[pal-mec-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1033-L1060
[pal-gfx-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L951-L972
[mesa-mec-selection]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1275-L1281
[mesa-si-indirect]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/gfx/si_compute.c#L766-L784
[pal12-base]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L4138-L4157
[mesa-meta-counts]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/nir/radv_meta_nir.c#L1391-L1425
[mesa-meta-dispatch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy_indirect_cs.c#L139-L190
[pal12-pfp-interleaved]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L1109-L1130
[pal12-me-interleaved]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L909-L968
[pal12-interleaved-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1035-L1082
[pal12-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_pm4_it_opcodes.h#L176-L177
