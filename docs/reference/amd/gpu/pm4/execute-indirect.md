# Execute-indirect compute commands

EXECUTE_INDIRECT_V2 lets the command processor consume argument records,
update shader user data, and dispatch the currently bound compute pipeline.
Inline metadata describes register locations, spill-copy ranges and the
launch-tuple offset within each record. A GPU-written count can select the
number of records without a host reading it or recording another dispatch. Argument
records, command metadata and firmware spill storage have separate owners.
[Compute caller][p12-compute-packet] [Metadata producer][p12-populate]
[MEC builder][p12-mec-builder]

## Applicability and selected paths

| PAL path | Selected mechanism |
| --- | --- |
| Compute queue in the `gfx9` implementation | Shader-generated command blocks, followed by the documented join/acquire/REWIND sequence. The generator's packet-capability flag does not redirect this method to PFP V2. |
| Dispatch on an older universal queue | The generator and pipeline predicates below select the packet path; V2 additionally requires the effective setting to be at least `ExecuteIndirectV2`. It uses the PFP representation with compute shader type. |
| GFX12 compute queue | `CmdExecuteIndirectCmds` calls `ExecuteIndirectPacket`, then the MEC `BuildExecuteIndirectV2Ace`. This entry does not consult the older packet-enable setting. |
| Ordinary dispatch on a GFX12 universal queue | PFP V2 with compute metadata. Hybrid task/mesh use on ACE is a separate protocol. |

[Older compute][p11-compute-entry] [Older universal selection][p11-universal-entry]
[Older packet choice][p11-universal-packet] [GFX12 compute][p12-compute-entry]
[GFX12 universal][p12-universal-entry]

For the older Dispatch generator, `enableExecuteIndirectPacket` must be set
and `useExecuteIndirectPacket` must reach
`UseExecuteIndirectV1PacketForDrawDispatch`. The universal caller then rejects
the packet path if task shaders or a mapped `numWorkGroupsRegAddr` are present,
or if its graphics spill check finds a nonempty spill table while the setting
is below `UseExecuteIndirectV1PacketForDrawSpillTable`. These are the source's
actual bound-state checks, including its graphics-state check on this shared
entry. [Generator predicate][p11-generator-create]
[Pipeline predicates][p11-universal-entry]

In the older implementation, PAL's FAMILY_NV3 RS64 branch advertises V2 at
PFP version 2060. Its settings loader initially downgrades Navi3 to
`ExecuteIndirectV1DrawDispatch`; subsequent settings reads can override that
default. Hardware/firmware capability, default policy and the actual selected
queue path are distinct conditions. Opcode presence alone selects none of
them. [Firmware branch][p11-native-support] [Default override][p-settings-default]
[Settings order][p-settings-init] [Generator selection][p11-generator-create]

V2 reuses the bound program, resource state and compiler ABI. The ordinary
operation is `dispatch=2`; the MEC declaration also names `dispatch_taskmesh=5`.
Both PFP revisions additionally declare `draw=0`, `draw_index=1`,
`dispatch_mesh=3` and `dispatch_rays=4`; GFX12 PFP adds `dispatch_taskmesh=5`.
Those declarations do not turn an ordinary dispatch record into a program-binding
token. The older shader-generation path and its storage owners are described
in [command buffers](command-buffers.md#pal-shader-generated-blocks).
[MEC declarations][p12-mec-v2] [GFX12 PFP declarations][p12-pfp-v2]
[Older PFP declarations][p11-pfp-v2] [Selected metadata][p12-populate]

## Argument records and count

PAL's public call receives a generator, argument GPU address, maximum count
and optional count GPU address. The generator owns the record layout. The
packet captures addresses and metadata; it does not copy the records.
Both addresses require four-byte alignment. [Public contract][p-execute-api]

PAL requires the command buffer to begin with `optimizeOneTimeSubmit` or
`optimizeExclusiveSubmit`: simultaneous command generation by the same buffer
on multiple queues can race. This is the public caller contract, not a claim
that V2 firmware enforces submission exclusivity. [Submission mode][p-execute-api]

| Input | Selected meaning |
| --- | --- |
| Argument address | Base of the sequence's records. |
| Byte stride | Distance between records, from the generator's `argBufStride`. |
| Maximum count | Unsigned 32-bit upper bound captured in the packet. |
| Count address zero | Use the captured maximum count. |
| Count address nonzero | Fetch the count from memory and clamp it to the maximum, according to the public API contract. |
| Dispatch parameter | Three unsigned 32-bit workgroup dimensions at the parameter's byte offset within each record. |
| User-data parameters | Values mapped to the bound pipeline's registers or spill locations, independently of the dispatch tuple. |

[Capture][p12-compute-packet] [Parameter layout][p12-param-init]
[Dispatch argument type][p-dispatch-args]

XGL's EXT generated-command adapter binds the compute pipeline and forwards
`indirectAddress`, `maxSequenceCount` and `sequenceCountAddress`. Its layout
builder maps a dispatch token to `DispatchIndirectArgs`, push constants to
PAL user-data ranges, and the requested stride to the generator. This is an
actual application caller, not an independent firmware implementation.
[XGL command][xgl-caller] [XGL layout][xgl-layout]

The private `countBufferAddr` comment also says a zero count value uses the
maximum. That differs from the public contract: the builder enables indirect
count solely from whether the address is nonzero. The private comment does
not establish a special firmware meaning for a fetched zero.
[Private comment][p12-packet-input] [Public rule][p-execute-api]
[Enable construction][p12-mec-builder]

## Base representations

The older GFX11 PFP, GFX12 PFP and GFX12 MEC base records are all 13 DWORDs.
Variable metadata and a three-DWORD operation follow, so 13 is not the packet
length. Word numbers below are zero-based: PAL's `ordinal1` is word 0.
The builders zero-initialize their records before filling selected fields.
[Older PFP][p11-pfp-v2] [GFX12 PFP][p12-pfp-v2] [GFX12 MEC][p12-mec-v2]

| Word and bits | Field and complete view |
| --- | --- |
| 0, 31:30; 29:16; 15:8 | Type 3; `count` is total packet DWORDs minus two; opcode `IT_EXECUTE_INDIRECT_V2 = 0xb4`. |
| 0, 7:0 | PFP: predicate bit 0, shader type bit 1, reset-filter-CAM bit 2, reserved bits 7:3. MEC definition: reserved low byte. |
| 1, 0 | `count_indirect_enable`. |
| 1, 5:1 | `userdata_dw_count`; MEC and PFP builders use different count units below. |
| 1, 6 | `command_index_enable`. |
| 1, 7 | PFP `userdata_gfx_register_enable`; MEC reserved. |
| 1, 9:8 | `num_spill_regs`. |
| 1, 12:10 | `init_mem_copy_count`. |
| 1, 15:13 | PFP `build_srd_count`; MEC reserved. |
| 1, 18:16 | `update_mem_copy_count`. |
| 1, 21:19 | `operation`: ordinary dispatch is 2. |
| 1, 22 | PFP `fetch_index_attributes`; MEC reserved. |
| 1, 25:23 | `userdata_scatter_mode`: MEC declares `cs_only=0`; PFP declares `cs_only=0`, `gs_only=0`, `ps_only=0`, `ps_gs=1`, `ps_gs_hs=2`. Ordinary compute selects CS. |
| 1, 30:26 | Older PFP: reserved bits 29:26 and `vertex_bounds_check_enable` bit 30. GFX12 PFP: `bottom_of_pipe_ts_after_draw` bit 26, reserved bits 28:27, `vertex_offset_mode_enabled` bit 29, bounds-check bit 30. MEC: all reserved. |
| 1, 31 | `thread_trace_enable`. |
| 2, 31:2; 3, 15:0 | `count_addr_lo`, `count_addr_hi`: address bits 31:2 and 47:32. Word 2 bits 1:0 and word 3 bits 31:16 are reserved. |
| 4, 31:0 | `max_count`. |
| 5, 31:0 | `stride`, in bytes. |
| 6, 31:2; 7, 15:0 | `data_addr_lo`, `data_addr_hi`: argument address bits 31:2 and 47:32. Word 6 bits 1:0 are reserved. |
| 7, 31:16 | PFP `index_attributes_offset`; MEC reserved. |
| 8, 7:0; 15:8; 31:16 | PFP `userdata_gfx_register`, reserved byte, `userdata_offset`. MEC reserves bits 15:0 and retains the user-data byte offset in bits 31:16. |
| 9, 31:2; 10, 15:0 | `spill_table_addr_lo`, `spill_table_addr_hi`: initial spill address bits 31:2 and 47:32. Word 9 bits 1:0 and word 10 bits 31:16 are reserved. |
| 11, 15:0; 31:16 | PFP `vb_table_size`, `spill_table_stride`. MEC reserves bits 15:0 and retains the spill-slot byte stride in bits 31:16. |
| 12 | PFP graphics: `spill_graphics_reg0/1/2` in successive low bytes, high byte reserved. PFP compute alternative and MEC: `spill_compute_reg0` in bits 15:0 and `spill_compute_reg1` in bits 31:16. |

[Complete field definitions][p12-mec-v2] [PFP differences][p12-pfp-v2]
[Older PFP definition][p11-pfp-v2] [Header helper][p12-type3]
[PFP header][p12-pfp-header] [Opcode][p12-opcode]

These packet addresses represent bits 47:0, despite the API's 64-bit address
type. The separate SET_BASE for global spill has its own representation;
neither that width nor the API type expands these embedded address fields.
Native mapping admission remains a separate condition.
[Address construction][p12-mec-builder] [Global base builder][pal-set-base]

The selected MEC header helper sets `ShaderCompute` at bit 1 even though the
generated MEC header names the low byte reserved. It leaves CAM reset clear;
the PFP callers set CAM reset. When the GFX12 compute-queue caller requests
packet predication, it wraps V2 in COND_EXEC with its final DWORD count and passes
`PredDisable` to the V2 builder. These source-specific emitted values do not
establish interchangeable MEC/PFP header semantics.
[MEC header][p12-mec-header] [Builders][p12-mec-builder]
[Predicate wrapper][p12-compute-packet] [PFP builder][p12-pfp-builder]

The GFX12 universal caller instead passes `PacketPredicate()` to its PFP V2
builder. It disables command-stream preemption when `cpPfpVersion < 2550`;
PAL identifies that version as the fix for saving/restoring the V2 SET_BASE
state. [Universal packet path][p12-universal-packet]
[Firmware constant][p12-meta-constants]

## Variable metadata and operation

MEC appends initialization copies, update copies, register runs, then one
operation record. PFP inserts any BuildSRD records before user data; ordinary
compute dispatch has none. All this metadata is serialized inline, rather
than retaining pointers to the recorder's temporary C++ objects.
[MEC append sequence][p12-mec-builder] [PFP append sequence][p12-pfp-builder]

### Spill-copy lists

Each list contains source byte offsets, destination byte offsets, and sizes
in DWORDs. The update source offsets are relative to the user-data argument
base. Initialization copies retain unchanged initial-spill values; updates
take changed values from the argument record. The generator derives both
lists from the pipeline mapping and spill threshold.
[List construction][p12-copy-lists] [Initialization][p12-copy-init]
[Updates][p12-copy-update] [User-data mapping][p12-user-data]

The packer handles two descriptors at a time, low component first:

```text
source[0] | source[1] << 16
destination[0] | destination[1] << 16
size_in_dwords[0] | size_in_dwords[1] << 16
```

The next pair follows those three words. An odd final descriptor uses zero
for each unused high component. Thus a list of `n` descriptors occupies
`3 * ceil(n / 2)` DWORDs; it is not an array of unpacked three-DWORD structs.
[Packing implementation][p12-packed] [Older equivalent][p11-packed]

### Register updates

MEC coalesces consecutive register locations. Each emitted DWORD stores a
16-bit register count in bits 15:0 and a 16-bit starting offset in bits 31:16.
Offsets are relative to `PERSISTENT_SPACE_START`. For example:

```text
locations: 0x244, 0x245, 0x246, 0x248, 0x249, 0x24c
runs:      0x02440003, 0x02480002, 0x024c0001
```

Here `userdata_dw_count` is three although six registers change. PFP compute
instead packs pairs of 16-bit register offsets and puts the original
argument-position span in `userdata_dw_count`. Values come from the record
at `userdata_offset`. The producer zeroes unused mapping positions; MEC
compaction does not remove zero holes. The helper alone does not establish
the firmware's interpretation of such holes.
[MEC runs][p12-mec-runs] [Location contract][p12-meta-constants]
[Mapping producer][p12-user-data] [PFP writer][p12-pfp-builder]

The example stays within `COMPUTE_USER_DATA_0..15` at `0x2e40..0x2e4f`,
relative to `PERSISTENT_SPACE_START=0x2c00`. PAL's compute bank has 16
registers; arbitrary 16-bit packed offsets do not expand that bank.
[Register addresses][p12-compute-registers] [Register base][p12-register-base]
[Bank capacity][p12-compute-bank]

### Dispatch tail

| Tail word | Older PFP dispatch | GFX12 PFP and MEC dispatch |
| --- | --- | --- |
| 0 | `dataOffset`, full 32-bit byte offset of the dispatch tuple. | Same. |
| 1 | Reserved bits 15:0; `commandIndex` in bits 31:16. | `numWorkGroup` in bits 9:0, enable bit 10, reserved bits 15:11, `commandIndex` in bits 31:16. |
| 2 | Full `COMPUTE_DISPATCH_INITIATOR`. | Same register, constructed from this caller's options. |

[Older operation record][p11-meta-records] [GFX12 record][p12-op-records]

The GFX12 compute-queue producer sets COMPUTE_SHADER_EN and FORCE_START_AT_000.
Wave32 and tunneling come from the current compute pipeline/queue options;
this caller disables ping-pong and 2D interleave. Its zero-initialized tail
leaves ORDER_MODE clear. Those are not the defaults of the ordinary
DISPATCH_INDIRECT builder. [Options][p12-compute-packet]
[Tail construction][p12-populate] [Initiator fields][p12-initiator]

Ordinary dispatch on the GFX12 universal/PFP path instead derives ping-pong
from `GetDispatchPingPongEn()` and passes its selected 2D-interleave option.
The shared metadata producer consumes those caller-specific options.
[Universal options][p12-universal-packet] [Tail construction][p12-populate]

The shared producer enables workgroup-location fields at
`pfpUcodeVersion >= 2810`, even for its ACE caller, when the location is mapped.
That is the actual PFP-version predicate; substituting an MEC-version rule
would change its meaning. Shader-visible workgroup inputs still have to match
the compiler ABI. The command-index helper selects a mapped increment-constant
register when present; its name alone supplies no initial-value or arithmetic
firmware contract. [Version constants][p12-meta-constants]
[Population][p12-populate] [Command index][p12-command-index]

## Spill backing and queue ownership

| Storage | Construction and purpose | Final owner |
| --- | --- | --- |
| Embedded initial spill | If the pipeline's spill threshold is at most the generator's user-data watermark, the recorder copies its current user-data entries into embedded command-allocator storage. The packet carries this address. | The command allocator and every CP/shader use of this image. |
| Queue-global spill | Submission lazily allocates GPU-only, always-resident storage in PAL's `DescriptorTable` VA range and programs SET_BASE index `execute_indirect_v2`. Firmware uses it for changing spill data across records. | The compute queue context through completed queue use and native teardown. |

[Initial image][p12-compute-preprocess] [Requirement mark][pal-spill-mark]
[Allocation][pal-allocation] [Submit selection][pal-queue-update]
[Preamble][pal-preamble] [Context destructor][pal-context-destroy]

GFX12 rounds spill-slot stride to 256 bytes. PAL explains the boundary using
CP's direct MALL access: shader reads can fetch a cache line covering an
adjacent slot before CP fills it. Slots aligned to the larger of the 64-byte
scalar-cache and 256-byte GL2 lines avoid that overlap. The older helper uses
128-byte alignment. These selected layouts do not replace a general
producer/consumer dependency. [GFX12 alignment rationale][pal-alignment]
[Older stride construction][p11-pfp-builder] [Older alignment][p11-alignment]

The GFX12 queue allocation is `1 MiB + 32 KiB`, also 256-byte aligned.
The source calls its 1 KiB-per-command model an approximation and the extra
32 KiB a hardware-workaround pad. This is allocation policy, not an
architectural maximum number of live dispatches. [Allocation][pal-allocation]

Even a packet with no spill updates marks the global requirement: the compute
caller says firmware expects the full allocation for a workaround. The
metadata helper's removal of initialization copies when updates are absent
does not remove this caller-level requirement. Marking the command buffer
performs no allocation; queue submission owns it.
[Unconditional mark][p12-compute-packet] [Metadata optimization][p12-copy-lists]
[Allocation owner][pal-queue-update]

The demonstrated lazy-allocation path scans the flags of directly submitted
command buffers. A nested buffer's V2 recording alone does not establish how
that separate requirement reaches the submitted parent; the cited scan
does not traverse nested buffers. [Submission scan][pal-queue-update]

The compute preamble waits for its exclusive execution cell to become zero,
writes one, invalidates scalar/instruction caches and installs the global
spill base. The postamble releases zero after pipeline completion with GL2
writeback/invalidation and vector-cache invalidation. These operations
serialize this context's reused state across the selected submissions.
[Preamble][pal-preamble] [Postamble][pal-postamble]

PAL's `Queue::Destroy()` calls `WaitIdle` before native queue/context destruction,
then the compute context frees global spill. This path does not check
the wait result. This establishes the intended completed-use path, not
successful retirement after a failed native wait. [Queue destruction][pal-queue-destroy]
[Wait contract][pal-wait-contract] [Native wait][pal-native-wait]

## Publication and last use

The public indirect call requires `PipelineStageFetchIndirectArgs` and
`CoherIndirectArgs` for input dependencies. On GFX12, CP bypasses GL2, so
shader-produced arguments/counts need the producer join and GL2 writeback
before CP consumption. Shader readers have their own cache acquire.
[Public dependency][p-execute-api]
[GFX12 cache rules](cache.md#gfx12-cp-and-shader-handoffs)

The complete flow for a directly submitted GFX12 compute command buffer is:

1. Establish the native queue, compatible pipeline and mappings. Retain
   command metadata, count, records, initial spill, code and payload for their
   actual readers; publish CPU-authored inputs before submission.
2. Write complete records and the intended count. Join the producer and apply
   its CP-facing cache operations. A count bound does not make concurrently
   changing records an atomic snapshot.
3. Record V2 with the generator's offsets, mappings, counts and operation
   tail, marking the global-spill requirement. At submission, the queue
   prepares that backing and its preamble before publishing commands for
   GPU execution.
4. CP consumes the selected records and launches the bound pipeline. Shader
   argument/spill readers can remain active after packet consumption.
5. Join the launched work and publish outputs for their next observer. Retain
   borrowed payload through any downstream consumer's completion.
6. Reuse command/embedded storage through its completion owner; preserve
   global spill through the queue's native completed-use lifecycle.

[Input contract][p-execute-api] [Recording][p12-compute-packet]
[Queue submission][pal-queue-submit]
[Command-storage retirement](command-buffers.md#cpu-rebuild-after-completed-use)

PAL's compute postamble drains outstanding CP DMA and, with busy tracking,
joins shaders before incrementing its command tracker. Its comment relies on
the scheduled native cache-flushing EOP for host visibility. Exhausting the
count, parsing past V2, and advancing a ring read pointer do not establish
the last-reader condition. [Command postamble][pal-command-postamble]

## Minimal record and representation limits

For one dispatch parameter at offset zero, stride 12, no user-data updates,
no spill, no command index or tracing, unmapped workgroup location, untunneled
wave64, and no count address, MEC emits 16 DWORDs: 64 bytes.
Let `A` be the aligned argument address and `N` the maximum count:

```text
word 0:       0xc00eb402          // type 3, count 14, opcode 0xb4, compute
word 1:       0x00100000          // operation dispatch=2
words 2–3:    0, 0               // count address disabled
word 4:       N
word 5:       12                 // byte stride
words 6–7:    A[31:0], A[47:32]
words 8–12:   0, 0, 0, 0, 0
word 13:      0                  // dispatch tuple byte offset
word 14:      0                  // no workgroup/index location
word 15:      0x00000005          // shader enable, force start at zero
```

This is builder-derived storage arithmetic. Program state, the global-spill
workaround, publication and completion remain part of the submission; the
example supplies neither an execution latency nor independent firmware
admission. [Builder][p12-mec-builder] [Tail][p12-populate]
[Header][p12-type3] [Caller][p12-compute-packet]

For initialization count `I`, update count `U`, PFP input span `D` and MEC run
count `R`, ordinary compute packet extents are:

```text
MEC: 16 + 3*ceil(I/2) + 3*ceil(U/2) + R DWORDs
PFP: 16 + 3*ceil(I/2) + 3*ceil(U/2) + ceil(D/2) DWORDs
```

[Packers][p12-packed] [MEC runs][p12-mec-runs]
[MEC extent][p12-mec-builder] [PFP extent][p12-pfp-builder]

The software capacities and encodings have distinct limits:

| Boundary | Source facts |
| --- | --- |
| Copy counts | Each field is three bits and receives a raw count. Helper arrays have eight entries; there is no count-minus-one conversion. Array capacity does not establish an encoding for eight or a bound on every public generator. |
| Packed components | Offsets and sizes narrow to 16 bits in the packer. Byte offsets and DWORD sizes retain different units. |
| Compute user-data span | GFX12 asserts a span of at most 16 argument positions. Older V2 asserts at most 32, while its raw field is five bits. These sources do not establish a zero-means-32 firmware rule. |
| User-data capacity | The API constant is 160 entries. The helper has a 256-entry LUT; its comment equating that length to the API maximum differs from the current constant. |
| Validation | The common generator validator checks terminal-operation placement, binding restrictions, one-entry increment parameters and total bytes versus stride. It does not prove every final metadata count or narrowed offset fits. |

[Fields][p12-mec-v2] [Arrays and LUT][p12-meta-records]
[Array capacities][p12-meta-constants]
[Mapped span][p12-user-data] [Older producer][p11-populate]
[Actual API capacity][p-userdata-cap] [GFX12 assignment][p12-userdata-cap]
[Validator][p-validator]

Validation through the size query is conditional on a non-null result output;
GFX12 creation constructs directly, and the older create checks the common
validator only in its assertions-enabled build. Client preconditions and
debug assertions therefore do not describe a universal runtime parser for
arbitrary metadata. The source differences above require their actual
producer or firmware contract, rather than an interpretation inferred from
array sizes. [GFX12 size query][p12-size-query] [Older size query][p11-size-query]
[GFX12 creation][p12-create]
[Older creation][p11-create]

[p12-compute-packet]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L774-L852
[p12-populate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12IndirectCmdGenerator.cpp#L419-L644
[p12-mec-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L3195-L3292
[p11-compute-entry]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1444-L1546
[p11-universal-entry]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L10801-L10851
[p11-universal-packet]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L10397-L10542
[p12-compute-entry]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L855-L863
[p12-universal-entry]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L7488-L7523
[p11-native-support]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L6075-L6110
[p-settings-default]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/settingsLoader.cpp#L87-L165
[p-settings-init]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/settingsLoader.cpp#L53-L67
[p11-generator-create]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9IndirectCmdGenerator.cpp#L110-L169
[p12-mec-v2]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1392-L1568
[p-execute-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4564-L4591
[p12-param-init]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12IndirectCmdGenerator.cpp#L107-L205
[p-dispatch-args]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L1404-L1410
[xgl-caller]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/vk_cmdbuffer.cpp#L3750-L3860
[xgl-layout]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/vk_indirect_commands_layout.cpp#L666-L834
[p12-packet-input]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.h#L166-L184
[p11-pfp-v2]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L4322-L4517
[p12-pfp-v2]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L2326-L2524
[p12-type3]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L184-L201
[pal-set-base]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2599-L2627
[p12-mec-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L43-L54
[p12-pfp-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L3025-L3190
[p12-copy-lists]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ExecuteIndirectCmdUtil.cpp#L170-L205
[p12-copy-init]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ExecuteIndirectCmdUtil.cpp#L253-L283
[p12-copy-update]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ExecuteIndirectCmdUtil.cpp#L286-L325
[p12-user-data]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12IndirectCmdGenerator.cpp#L209-L413
[p12-packed]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ExecuteIndirectCmdUtil.cpp#L48-L98
[p11-packed]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ExecuteIndirectCmdUtil.cpp#L47-L99
[p12-mec-runs]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ExecuteIndirectCmdUtil.cpp#L101-L147
[p12-op-records]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ExecuteIndirectCmdUtil.h#L112-L194
[p11-meta-records]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ExecuteIndirectCmdUtil.h#L37-L172
[p12-initiator]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_registers.h#L2299-L2326
[p12-meta-constants]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ExecuteIndirectCmdUtil.h#L37-L110
[p12-command-index]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ExecuteIndirectCmdUtil.cpp#L328-L361
[p12-compute-preprocess]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L723-L769
[pal-spill-mark]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L1977-L1993
[pal-allocation]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12QueueContexts.cpp#L460-L495
[pal-queue-update]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12QueueContexts.cpp#L1410-L1500
[pal-preamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12QueueContexts.cpp#L1512-L1565
[pal-context-destroy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12QueueContexts.cpp#L1320-L1328
[pal-alignment]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Chip.h#L244-L254
[p11-pfp-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1373-L1535
[pal-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12QueueContexts.cpp#L611-L633
[pal-queue-destroy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/queue.cpp#L338-L425
[pal-wait-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palQueue.h#L467-L472
[pal-native-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuQueue.cpp#L1642-L1671
[pal-queue-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12QueueContexts.cpp#L1350-L1399
[pal-command-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L948-L980
[p12-meta-records]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ExecuteIndirectCmdUtil.h#L196-L241
[p11-populate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L9865-L10208
[p-userdata-cap]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.h#L67-L70
[p12-userdata-cap]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Device.cpp#L174-L194
[p-validator]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/indirectCmdGenerator.cpp#L38-L107
[p12-size-query]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Device.cpp#L1769-L1780
[p11-size-query]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L2954-L2965
[p12-create]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Device.cpp#L1783-L1792
[p11-create]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L2968-L2981
[p12-universal-packet]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L7317-L7485
[p12-pfp-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L43-L57
[p12-opcode]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_pm4_it_opcodes.h#L183-L190
[p12-compute-registers]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_offset.h#L252-L267
[p12-register-base]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_enum.h#L9113-L9114
[p12-compute-bank]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Chip.h#L230-L231
[p11-alignment]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Chip.h#L157-L167
