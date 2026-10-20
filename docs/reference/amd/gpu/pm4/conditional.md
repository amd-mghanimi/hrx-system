# Conditional compute execution

The command processor can use a memory value to select which commands it
executes. `COND_EXEC` guards a counted inline range; `COND_INDIRECT_BUFFER`
selects an indirect command block. These operations sample a condition when
reached. A producer dependency establishes when that sample is valid, and a
separate completion protocol establishes when the selected work has finished.
[Inline consumer][radv-predicate] [Conditional-block contract][pal-if-api]

## Mechanisms and applicability

| Mechanism | Decision input and effect | Source scope |
| --- | --- | --- |
| `COND_EXEC`, opcode `0x22` | A 32-bit memory value controls execution of the following counted DWORD range; zero skips it. | PAL's older and GFX12 compute builders; Mesa's common emitter and MEC callers. |
| `COND_INDIRECT_BUFFER`, opcode `0x3f` | A masked memory operand and reference select a pass/fail indirect command block. | PAL's structured `If`/`Else` and `While` construction, with older and GFX12 builders. |
| `PRED_EXEC`, opcode `0x23` | A virtual-XCC selection mask controls which CP instance executes a counted range. | ROCr's multi-XCC PC-sampling command stream. |
| `SET_PREDICATION`, opcode `0x20`, and packet-header predication | Graphics-engine predicate state controls eligible predicated packets. | PAL PFP builder and RADV general-queue path; their compute callers use `COND_EXEC` emulation. |

[PAL opcodes][pal-opcodes] [GFX12 opcodes][pal12-opcodes]
[Compute Boolean setup][pal-predicate] [Mesa engine selection][radv-begin]
[PFP predicate builder][pal-set-predication]
[Virtual-XCC fields][rocr-fields] [Virtual-XCC caller][rocr-xcc]

The layouts below come from PAL's `gfx9` merged MEC definitions and its
separate GFX12 definitions. They identify source-visible representations;
the transport, firmware and physical engine still determine native admission.
The virtual-XCC form is tied to ROCr's `NumXcc > 1` caller, rather than inferred
from an RDNA compiler-target name. [Architecture identities](../architectures.md)
keeps those distinctions explicit.

## Inline ranges: COND_EXEC

PAL's two compute builders emit five DWORDs. Mesa uses this length for
`gfx_level >= GFX7`; its older path emits four DWORDs and omits the control
word. The five-DWORD MEC representation is:

| Word | Bits | Field and meaning |
| --- | --- | --- |
| 0 | 31:30, 29:16, 15:8 | Type 3, count 3, opcode `0x22`. PAL leaves the remaining header bits zero. |
| 1 | 31:2 | `addr_lo`, low predicate byte address; bits 1:0 are reserved, giving four-byte alignment. |
| 2 | 31:0 | `addr_hi`, high predicate address word. |
| 3 | 26:25 | Older `cache_policy` or GFX12 `temporal`, described below. Other bits are reserved. |
| 4 | 13:0 | `exec_count`, number of following DWORDs controlled by the condition. Bits 31:14 are reserved. |

[Older layout][pal-cond-fields] [GFX12 layout][pal12-cond-fields]
[Common MEC header][pal-header-fields]
[Older builder][pal-cond-build] [GFX12 builder][pal12-cond-build]
[Mesa generation split][mesa-cond-build]

The count covers the guarded commands, excluding `COND_EXEC` itself. Its
14-bit representation has a maximum of `0x3fff` DWORDs; that capacity does
not establish valid command boundaries or permit a range beyond its backing.
PAL guards a direct dispatch and its optional trace marker together. RADV's
indirect compute path includes both the dispatch and, when selected, all three
alignment-repair copies in the count. These callers count complete packet
sequences, not bytes or dispatched workgroups. [PAL dispatch range][pal-dispatch]
[RADV indirect range][radv-dispatch]

The two policy enumerations occupy the same bits but use different vocabulary:

| Value | Older `cache_policy` | GFX12 `temporal` |
| --- | --- | --- |
| 0 | LRU | RT |
| 1 | STREAM | NT |
| 2 | NOA | HT |
| 3 | BYPASS | LU |

Both PAL builders zero-initialize the packet and leave this operand zero.
The identical bit position does not make the other values interchangeable
across definitions. A policy operand also supplies no producer execution
dependency or consumer payload acquire. [Policy definitions][pal-cond-fields]
[GFX12 policy definitions][pal12-cond-fields]

### Boolean normalization and sampling

PAL supports Boolean32 and Boolean64 in its compute `CmdSetPredication` path.
It allocates one private DWORD, initializes it on the CPU to the complement
of the requested polarity, then emits a conditional confirmed `WRITE_DATA`
which writes the polarity when the source DWORD is nonzero. Boolean64 repeats
the conditional/write pair for the upper DWORD. A nonzero half writes the
requested polarity; two zero halves leave its initial complement. Later
dispatch guards read the private word.
[Older setup][pal-predicate] [GFX12 setup][pal12-predicate]
[Write confirmation][pal-write]

The source value is read by those normalization commands, rather than freshly
reduced from both halves at every later dispatch. Two 32-bit reads are not
evidence of an atomic 64-bit snapshot. A producer retains a stable value
through the reads that consume it. The private predicate has an independent
lifetime through all guards that reference it.

RADV's ordinary MEC guard reads the user's predicate directly. For inverted
conditions, the first needed guard emits a confirmed write of one to an
upload word, conditionally writes zero when the source is nonzero, and then
uses that upload word for subsequent guards. Its emitted-state flag resets
at conditional-region begin/end. The unconditional initialization is in the
GPU command stream and therefore executes with that sequence on replay.
[Inversion sequence][radv-predicate] [Region state][radv-region]
[Confirmed immediate copy][radv-copy]

These are distinct sampling strategies. A dynamic scheduler which needs a new
decision at each stage makes each predicate-read dependency explicit; a cached
Boolean normalization is not an implicit subscription to later memory writes.
RADV's source also explains that its conditional-rendering API permits
implementation-dependent decisions if the value changes within an active
region. That API allowance does not establish a general queue-control protocol.
[Caller explanation][radv-begin]

The command sizes follow directly from these builders: a five-DWORD inline
guard adds 20 bytes. PAL's Boolean32 setup adds one five-DWORD guard and one
five-DWORD single-value write, or 40 command bytes; Boolean64 uses two pairs,
or 80 bytes. RADV's inversion uses two six-DWORD copies and one five-DWORD
guard, or 68 bytes before the guarded operation. These are command-storage
costs, not measured execution latencies. [PAL setup][pal-predicate]
[Write-data construction][pal-write-size] [RADV inversion][radv-predicate]

### Submission and mutable predicate storage

PAL's private predicate occupies persistent embedded GPU backing. Its allocator
enables separate CPU staging only for command allocations, not embedded data;
the recording-time initializer therefore writes the backing subsequently used
by the GPU. `End` associates the embedded chunk with the command root for
lifetime tracking. The public embedded-data contract retains its GPU address
until reset or the next begin. [Allocation policy][pal-embedded-policy]
[Mapping and write pointer][pal-embedded-map] [Root association][pal-record-end]
[Embedded lifetime][pal-embedded]

The normal public submission path calls a no-op `PreSubmit`, increments the
command stream's busy-tracking count, and launches its recorded commands.
The optional upload path copies executable command ranges from command-stream
chunks; it does not copy a pristine embedded-data image. Neither mechanism
reinitializes the private predicate. Command upload, execution bookkeeping and
mutable-data initialization are separate operations.
[Pre-submit hook][pal-pre-submit] [Submission bookkeeping][pal-submit-owner]
[Counter owner][pal-count-owner] [Uploaded ranges][pal-upload-range]

XGL supplies a real reusable caller: its conditional-rendering translation
selects PAL Boolean32, its one-time flag comes from the application's command
buffer usage, and exclusive submission remains a separate optimization.
Its normal queue path submits the recorded PAL command-buffer object.
Thus this public compute sequence has no per-execution restore when a later
submission changes the source condition. It cannot alone establish a
changed-predicate replay protocol. This is a conclusion about the cited source
path, not a measured result for an installed driver or a closed client.
[Predicate translation][xgl-predicate] [Build flags][xgl-build-flags]
[Submitted object][xgl-submit-storage] [Native submission][xgl-submit-native]

Nested predicate inheritance has its own copy: before calling a nested compute
buffer, PAL emits a confirmed 32-bit `COPY_DATA` from the caller's normalized
word to the callee's inherited word. That propagates the caller's current
decision; it does not reset the caller or resample its original source.
A reusable normalization protocol includes initialization in the executing
sequence or a separately ordered restoration of uniquely owned storage.
[Older inheritance][pal-inherited] [GFX12 inheritance][pal12-inherited]

## Conditional indirect blocks

`COND_INDIRECT_BUFFER` shares opcode `0x3f` with ordinary `INDIRECT_BUFFER`
but has a fourteen-DWORD representation and different operands. PAL's older
builder spells the opcode `IT_INDIRECT_BUFFER`; GFX12 spells it
`IT_COND_INDIRECT_BUFFER`. Both enum values are `0x3f`.
[Older builder][pal-branch-build] [GFX12 builder][pal12-branch-build]
[Opcode aliases][pal-opcodes] [GFX12 aliases][pal12-opcodes]

| Word | Bits | Field and units |
| --- | --- | --- |
| 0 | 31:30, 29:16, 15:8 | Type 3, count 12, opcode `0x3f`; remaining bits zero in PAL's builder. |
| 1 | 1:0 | `mode`: 1 is if-then, 2 is if-then-else. |
| 1 | 10:8 | `function`, comparison selector. Other bits in this word are reserved. |
| 2 | 31:3 | `compare_addr_lo` in the generated layout, with bits 2:0 marked reserved; the builder's alignment exception is below. |
| 3 | 31:0 | `compare_addr_hi`. |
| 4–5 | 31:0 each | `mask_lo`, `mask_hi`, a 64-bit comparison mask; reference masking is discussed below. |
| 6–7 | 31:0 each | `reference_lo`, `reference_hi`, a 64-bit reference. |
| 8, 11 | 31:2 | `ib_base1_lo`, `ib_base2_lo`; low command byte addresses with four-byte alignment. Bits 1:0 are reserved. |
| 9, 12 | 31:0 | `ib_base1_hi`, `ib_base2_hi`. |
| 10, 13 | 19:0 | `ib_size1`, `ib_size2`, direct DWORD counts. |
| 10, 13 | 29:28 | Older `cache_policy1/2` or GFX12 `temporal1/2`; other bits outside the count are reserved. |

[Older fields and enums][pal-branch-fields]
[GFX12 fields and enums][pal12-branch-fields] [Mask contract][pal-if-api]

Comparison encodings are 0 always, 1 less, 2 less-or-equal, 3 equal,
4 not-equal, 5 greater-or-equal and 6 greater than the reference. There is no
never selector in these definitions. PAL implements its `Never` condition by
choosing `Always` and exchanging which branch receives each destination.
Its builder always chooses if-then-else and initially leaves both branch
addresses and lengths for later patching. [Comparison translation][pal-branch-build]
[Never construction][pal-if]

The generated comparison-address field suggests eight-byte alignment. Both
PAL builders explicitly state that four bytes suffice and write the full low
address word rather than assigning that bitfield. This is an attributed
builder exception, not a reason to silently rewrite the generated layout.
The IB count fields hold up to `0xfffff` DWORDs each; command allocation,
alignment and native transport constraints still bound actual use. Branch
cache-policy values use the same older/GFX12 naming split as the inline
packet, at their different bit positions. [Older fields][pal-branch-fields]
[GFX12 fields][pal12-branch-fields] [Builder exception][pal12-branch-build]

### Reference masking and portability

PAL's public contract describes applying the mask to GPU memory before
comparison: `(memory & mask) op reference`. Both packet builders copy mask
and reference independently, without masking the reference during encoding.
[API contract][pal-if-api] [Older builder][pal-branch-build]
[GFX12 builder][pal12-branch-build]

Native observations on Linux KFD user-managed compute queues distinguish two
behaviors when the reference contains bits outside the mask. The executed
predicates match these unsigned comparison models:

| Observed deployment | Comparison model |
| --- | --- |
| gfx1100 | `(memory & mask) op reference` |
| gfx1151 | `(memory & mask) op (reference & mask)` |

For example, with mask `0x00000000ffffffff`, masked memory
`0x0000000080000000` and reference `0x0000000180000000`, less-than selects
the pass arm under the first model and the fail arm under the second.
Experiments with masked-out reference bits inside either DWORD also match
the second model; ignoring only an entirely masked-out DWORD does not
explain those results. These observations use stable shader-produced
operands, joined before the branch, and retained command graphs with
unconditional completion. Both four-byte and eight-byte comparison-address
alignments are exercised.

This is evidence about those deployments, not a source-defined architecture
boundary. Firmware revisions were not captured, and the cited definitions
and builders do not identify a reference-masking exception. Neither the
packet layout nor a compiler target name determines which model another
hardware/firmware pairing implements.

A portable operand contract requires `reference & ~mask == 0`. The two
models then compare identical values for every comparison function, while
ignored memory bits remain unrestricted. A zero mask uses a zero reference.
This is a caller precondition, not an encoding transformation: silently
masking an arbitrary supplied reference can change the requested predicate,
as the less-than example demonstrates.

### Structured control flow and ownership

PAL ends the current command block with the conditional packet. Pass selects
IB1 and fail selects IB2. It patches each destination only when the complete
target block address and DWORD length are known. An `Else` ends the first arm
with a continuation chain; `EndIf` connects the arms to the following block.
A one-armed condition directs its unused arm to that continuation. This is
explicit command-block construction, not a saved shader register context.
[Conditional block construction][pal-if] [Branch patching][pal-patch]
[GFX12 patching][pal12-patch]

`While` gives its comparison block an aligned length and ends the body with
a chain back to that block. Each iteration reaches another comparison. The
body's writes still need the producer-to-fetch dependency before a following
iteration reads them. PAL's public conditional and loop contracts require
`PipelineStageFetchIndirectArgs` and `CoherIndirectArgs` for their input.
[Loop construction][pal-loop] [Loop memory contract][pal-loop-api]

PAL disables its PM4 command optimizer on entering structured control flow
because the optimizer does not model those paths. Register-state elimination
which is valid along one straight-line stream cannot assume that both arms
ran. The enclosing command owner also retains predicate storage, every
reachable body and each continuation through their possible fetches and
workload users. [Optimizer and branch construction][pal-if]
[Command publication and retirement](command-buffers.md#publication-and-memory-ownership)
describe the surrounding lifetime contract.

## Virtual-XCC selection: PRED_EXEC

ROCr's two-DWORD representation uses opcode `0x23`. Word 1 bits 13:0 contain
the following DWORD count, bits 31:24 contain `VIRTUALXCCID_SELECT`, and its
builder leaves the intermediate bits zero. For `NumXcc > 1`, the sampling
caller writes mask `0x1` to execute the guarded sequence on virtual XCC 0.
There is no memory-predicate address in this form. [Fields][rocr-fields]
[Selected caller][rocr-xcc]

### Participant index, mask and complete region

The profiling library's `BuildPredExecPacket` accepts a virtual participant
**index** and encodes `1 << xcc_select` into the eight-bit field. Its GFX9
and GFX12 builders emit the packet; its GFX10 and GFX11 methods are empty.
The PMC caller enables the wrapper when `xcc_number_` exceeds one, making
the reported topology part of this implementation's selection condition.
[GFX9 builder][profile-pred9] [GFX12 builder][profile-pred12]
[GFX10 method][profile-pred10] [GFX11 method][profile-pred11]

The PMC reader iterates the reported virtual XCCs, constructs a guarded read
sequence for each participant, and advances the result cursor between them. This
supplies a real caller for selecting indices beyond zero. Its AID-related
path instead advances by `xcc_per_aid_`; that grouping is supplied by the
profiling topology policy and is separate from the packet's mask encoding.
[Per-participant collection][profile-read-xcc]

The body count excludes the two prefix DWORDs and represents at most
`0x3fff` following DWORDs. `PrecExecBuilder` reserves the prefix, constructs
the body, then patches its count. For the library's packet-at-a-time
builders, `CmdBuffer::CheckPredExec` closes and reopens the region before an
append would cross that limit. Each new prefix preserves the participant
selection and starts at a packet boundary. Its explicit rejection of nested
regions is a library construction rule, not a hardware nesting guarantee.
[Region construction][profile-pred-region] [Append boundary][profile-pred-append]

PAL's generated PFP layout names the upper eight bits `device_select`.
That field spelling alone does not establish the compute caller's
virtual-XCC semantics for a graphics stream. Engine, carrier and actual
caller remain part of the operation's applicability. [PFP layout][pal-pred-pfp]

### Completion and retained storage

One guarded body contains both an atomic exchange and the copy of its returned
value. Selecting only one command would lose the same-CP result relationship.
Another guarded body contains the sample-buffer wait, cache work, copies and
written-count reset. Both use an AQL carrier with SYSTEM release and a separate
completion signal; the CPU waits before reading or advancing those buffers.
The selection mask does not replace those completion or ownership operations.
[Atomic result flow](atomics.md#retrieving-the-previous-value)
[Copy-body selection and completion][rocr-copy-body]

This source establishes a virtual-CP selection use. It supplies no general
physical-XCC mapping API, shader workgroup-placement promise or cross-XCC
memory barrier. Partition-relative virtual identity and queue-specific logical
workgroup distribution have [separate configuration owners](../scheduling.md#partition-virtual-ids-and-queue-logical-ids).
Carrier completion still protects every participant's possible command fetches.

## A complete conditional dispatch

A reusable command sequence keeps selection, payload ordering and retirement
separate:

1. The owners allocate the predicate, executable, arguments, input/output data
   and complete command ranges with the selected VM mappings and access paths.
2. The producer publishes the predicate and payload. The consumer stream joins
   that producer and performs the cache operations for CP predicate fetch and
   any later shader or DMA payload reads.
3. The CP reads the condition and selects a complete inline packet range or a
   complete indirect block. A zero/false decision advances along its skip/fail
   path; it does not wait for a future true value.
4. A reachable continuation joins every launched user and supplies the required
   release and confirmed completion. When an operation is optional, an
   unconditional completion path still establishes that the decision was made
   and all selected users finished.
5. The owner observes completion and retires all published command references
   before reusing their storage. Mutable normalized predicates have an explicit
   initialization for each intended execution epoch.

This sequence composes the source-defined selection with the ordinary
[cross-queue handoff](handoff.md) and
[command-storage lifetime](command-buffers.md) contracts. Its cache operations
depend on the actual producer, mapping and observer. The predicate read,
selected dispatch, shader completion and command retirement remain distinct
events. PAL's caller-visible barrier requirements and ROCr's separate
carrier completion make those boundaries explicit.
[Predicate dependency][pal-predicate-api] [Carrier completion][rocr-xcc]

[pal-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_pm4_it_opcodes.h#L45-L74
[pal12-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_pm4_it_opcodes.h#L45-L82
[pal-cond-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L380-L445
[pal-header-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L43-L55
[pal12-cond-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L315-L380
[pal-cond-build]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L877-L899
[pal12-cond-build]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2771-L2793
[mesa-cond-build]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L36-L54
[pal-predicate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1389-L1441
[pal12-predicate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L448-L500
[pal-write]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4676-L4692
[pal-write-size]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4600-L4675
[pal-set-predication]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4090-L4172
[pal-dispatch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L225-L261
[pal-predicate-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4166-L4194
[pal-embedded]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4444-L4458
[radv-predicate]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L11506-L11545
[radv-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L11495-L11504
[radv-dispatch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15098-L15135
[radv-begin]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16552-L16616
[radv-region]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16616-L16646
[pal-branch-build]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L901-L960
[pal12-branch-build]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2713-L2768
[pal-branch-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L447-L620
[pal12-branch-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L382-L555
[pal-if-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4207-L4231
[pal-loop-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4233-L4253
[pal-if]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdStream.cpp#L249-L340
[pal-loop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdStream.cpp#L342-L428
[pal-patch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdStream.cpp#L1477-L1515
[pal12-patch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdStream.cpp#L175-L214
[rocr-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_gpu_pm4.h#L50-L106
[rocr-xcc]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4739-L4799
[rocr-copy-body]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4820-L4950
[pal-embedded-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L195-L230
[pal-embedded-map]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L123-L166
[pal-record-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.cpp#L346-L379
[pal-pre-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.h#L772-L776
[pal-submit-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/queue.cpp#L701-L737
[pal-count-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L626-L649
[pal-upload-range]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/cmdUploadRing.cpp#L416-L484
[xgl-predicate]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/vk_cmdbuffer.cpp#L11041-L11071
[xgl-build-flags]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/vk_cmdbuffer.cpp#L1371-L1377
[xgl-submit-storage]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/vk_queue.cpp#L1543-L1548
[xgl-submit-native]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/vk_queue.cpp#L1208-L1218
[pal-inherited]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1637-L1677
[pal12-inherited]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L631-L667
[profile-pred9]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx9_cmd_builder.h#L80-L90
[profile-pred12]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx12_cmd_builder.h#L676-L688
[profile-pred10]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx10_cmd_builder.h#L262-L264
[profile-pred11]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/gfx11_cmd_builder.h#L392-L392
[profile-read-xcc]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/pmc_builder.h#L1187-L1209
[profile-pred-region]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/pmc_builder.h#L70-L129
[profile-pred-append]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocprofiler-sdk/source/lib/aqlprofile/pm4/cmd_builder.h#L106-L192
[pal-pred-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L3232-L3256
