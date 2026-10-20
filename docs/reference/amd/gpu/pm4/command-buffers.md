# PM4 command buffers

INDIRECT_BUFFER redirects command fetch to a caller-owned GPU address and
count. A primary compute ring uses CHAIN clear to call a first-level IB and
resume afterward. A compute IB uses CHAIN set to replace its remaining stream
with another IB; it cannot nest an IB2 call and return. This creates two memory
owners: the referring ring or command stream, and the separately referenced
command storage. A shader launched by that body may remain active after command
parsing has advanced. [Compute call and chain rules][pal-compute-chain]

The libhsakmt KFD utility provides a concrete first-level raw-compute caller:
`Dispatch::Submit` builds an IB, places its reference on the primary ring, and
then emits the event-producing release on that ring. The body separately joins
its shader before return. Its memory-copy caller waits for the event and
destroys the queue before releasing the dispatch and memory owners.
[Caller and cleanup][kfd-caller] [Submission][kfd-submit] [Body join][kfd-body]

[Queue publication](publication.md) describes primary-ring construction,
write-pointer visibility and doorbell protocols for KFD, scheduled DRM and
DRM user queues. Those transport rules surround the indirect-command
storage and entry/return protocol described here.

[Conditional execution](conditional.md) describes `COND_EXEC` inline ranges
and `COND_INDIRECT_BUFFER` pass/fail blocks. The conditional form shares the
ordinary IB opcode but has a different packet layout and comparison operands.
Its selected branches retain the command ownership described here.

## Representation and entry context

The ordinary INDIRECT_BUFFER form is four DWORDs, opcode `0x3f`. MEC and PFP
share the address/count words but interpret several control bits differently:

| Word | Fields and units |
| --- | --- |
| 0 | Type-3 header, count 2. |
| 1 bits 31:2 | `ib_base_lo`, the corresponding bits of the IB byte address. Modern PAL layouts reserve bits 1:0; KFD's older layout calls them `swap_function`, which its caller leaves zero. Both callers therefore use four-byte aligned addresses. |
| 2 | `ib_base_hi`. PAL's merged and GFX12 structures define all 32 bits; the older KFD structure and ROCr macros define bits 15:0 and leave the upper 16 bits zero. |
| 3 bits 19:0 | `ib_size`, direct DWORD count, not bytes or count-minus-one. Maximum field value `0xfffff` represents `0x3ffffc` bytes. |
| 3 bit 20 | `chain`: zero calls the IB and returns to the containing stream; one replaces the current IB's remaining stream. The legal entry level remains engine-specific. |
| 3 bits 31:21 | Engine- and source-specific control fields below. |

[KFD construction][kfd-packet] [KFD layout][kfd-layout]
[ROCr macros][rocr-ib-macros] [PAL MEC layout][pal-layout]
[PAL GFX12 MEC layout][pal12-layout]

| Word 3 bits | MEC compute view | PFP graphics view |
| --- | --- | --- |
| 21 | `offload_polling`. | `pre_ena`, preemption enable. |
| 22 | Reserved in PAL; older KFD calls it `volatile_setting`. | Reserved in PAL; Mesa's GFX11/GFX12 definitions call it `inherit_vmid`. |
| 23 | `valid`. | Reserved. |
| 27:24 | `vmid`. | `vmid`. |
| 29:28 | `cache_policy` in PAL's merged definition; `temporal` in its GFX12 definition. | Corresponding engine-specific policy field. |
| 30 | Reserved in PAL; Mesa's GFX11/GFX12 definitions call it `inherit_vmid`. | `pre_resume`, preemption resume. |
| 31 | Reserved in PAL; Mesa's GFX11/GFX12 definitions call it `priv`. | Reserved in PAL; Mesa calls it `priv`. |

[Merged MEC][pal-layout] [Merged PFP][pal-pfp-layout]
[GFX12 MEC][pal12-layout] [GFX12 PFP][pal12-pfp-layout]
[Mesa GFX11 MEC][mesa11-mec-ib] [Mesa GFX11 PFP][mesa11-pfp-ib]
[Mesa GFX12 MEC][mesa12-mec-ib] [Mesa GFX12 PFP][mesa12-pfp-ib]

The policy names are also source-specific. Equal numeric values do not make
these enumerations interchangeable:

| Value in bits 29:28 | Older KFD enum | PAL merged MEC/PFP | PAL GFX12 MEC/PFP |
| --- | --- | --- | --- |
| 0 | LRU | LRU | RT |
| 1 | STREAM | STREAM | NT |
| 2 | BYPASS | NOA | HT |
| 3 | No named value | BYPASS | LU |

[KFD policy values][kfd-policy] [Merged policy values][pal-layout]
[GFX12 policy values][pal12-layout]

The count's field capacity does not override the allocation extent, address
width or transport's command-size/alignment requirements. The complete
referenced interval must fit the selected address representation and remain
readable. An aligned entry alone does not establish those facts.

KFD emits header `0xc0023f02`, VALID=1 and numeric policy 2, with the other
controls zero. PAL's merged and GFX12 builders emit `0xc0023f00`, clear the
complete packet, and set the caller's address, DWORD count and CHAIN. Compute
sets VALID=1 and asserts that preemption was not requested; graphics instead
writes the caller's PRE_ENA. OFFLOAD, VMID, PRE_RESUME and the policy field
remain zero. The zero VMID operand does not mean the process executes in
hardware VMID zero: the queue/submission transport owns that context.
[KFD header][kfd-header] [KFD packet][kfd-packet]
[PAL builder][pal-builder] [GFX12 builder][pal12-builder]
[Header defaults][pal-header]

The source disagreement about reserved bits has a concrete transport use.
Mesa's opt-in userqueue path sets INHERIT_VMID along with VALID for its
compute-ring IB; graphics uses the distinct PFP INHERIT_VMID position.
Selection requires `AMD_USERQ`, a kernel-advertised per-IP userqueue mask and
DRM minor version at least 65. These controls belong to that native queue
protocol, rather than to every IB submitted through scheduled DRM.
[Userqueue selection][mesa-userq-selection] [Userqueue IB][mesa-userq-ib]

### Native transport extents

The packet's address granularity, a transport's advertised alignment and an
allocator's policy have distinct owners:

| Owner | Extent or alignment |
| --- | --- |
| Modern ordinary IB representation | Four-byte address granularity; 20-bit DWORD count. |
| Linux DRM GFX/COMPUTE hardware-IP query | Reports 32-byte IB start and size alignment. |
| Linux CS parser | Bounds `ib_bytes / 4` by the selected ring's packet capacity, then records that DWORD count and the supplied GPU VA. This function does not itself check the advertised 32-byte alignment. |
| Mesa allocation policy | Uses at least 256-byte allocation alignment, also respecting larger queried start/size alignments. Its GFX/COMPUTE body-padding mask is seven DWORDs, giving eight-DWORD boundaries. |
| Linux kernel ring padding | The inspected GFX/compute ring registrations use `align_mask=0xff`. Generic kernel IB padding and ring commit use that mask for 256-DWORD boundaries; this is separate from userspace IB query and allocation policy. |

[DRM properties][linux-ib-properties] [Parser][linux-ib-parser]
[Ring capacity][linux-ib-capacity] [Mesa padding][mesa-ib-padding]
[Mesa allocation alignment][mesa-ib-alignment]
[Kernel padding and commit][linux-ib-padding] [GFX11 registration][linux11-rings]
[GC12.1 registration][linux121-rings]

Scheduled DRM also owns the first-level packet. PAL's Linux `AddIb` passes
the first stream's byte address and converts its DWORD extent to `ib_bytes`;
the kernel chooses the VMID and emits the primary-ring reference. The compute
emitters preserve a generation difference:

| Linux implementation | Ordinary scheduled IB emission |
| --- | --- |
| GFX6 | High address masked to 16 bits; size and VMID control without the later VALID bit. |
| GFX7 / GFX8 compute | High address masked to 16 bits; size, selected VMID and VALID. |
| GFX9 / GFX9.4.3 compute | Full 32-bit high address; size, selected VMID and VALID. |
| GFX10 / GFX11 compute | Full 32-bit high address; size, selected VMID and VALID. |
| GFX12 / GC12.1 compute | Full 32-bit high address; size, selected VMID and VALID. |

[PAL native descriptor][pal-native-ib] [GFX6][linux6-ib]
[GFX7][linux7-ib] [GFX8][linux8-ib] [GFX9][linux9-ib]
[GFX9.4.3][linux943-ib] [GFX10][linux10-ib] [GFX11][linux11-ib]
[GFX12][linux12-ib] [GC12.1][linux121-ib]

Entry and return do not save and restore shader register bindings. Each
state-dependent body needs the compatible program, resource, geometry and
argument state it consumes. PAL's public command-buffer contract likewise
requires explicit state for independent submitted buffers. Nested graphics
IB2, first-level MEC IBs and [AQL vendor carriers](../aql/transfers.md) are
separate entry/context contracts. RADV inlines compute secondary buffers in
its ordinary secondary-execution path while allowing graphics IB2 execution.
[Command state contract][pal-reset-contract] [RADV secondary execution][mesa-secondary]

### PASID and constant-engine forms

INDIRECT_BUFFER_PASID, opcode `0x5c`, has five DWORDs and header count 3.
PAL's merged and GFX12 MEC layouts retain the ordinary address, size, CHAIN,
OFFLOAD_POLLING and VALID positions, but reserve word 3 bits 27:24 instead of
carrying VMID there. Word 4 supplies `pasid[15:0]`, with bits 31:16 reserved.
The policy field retains the corresponding merged or GFX12 enumeration;
word 3 bits 22 and 31:30 are reserved. This is a separate representation,
rather than a wider value written into ordinary IB's VMID field.
[Merged PASID layout][pal-pasid] [GFX12 PASID layout][pal12-pasid]
[Opcode values][pal-opcodes]

PAL also declares a GFX10-labelled CE INDIRECT_BUFFER_CONST, opcode `0x33`,
with four DWORDs and count 2. Its address and control positions match the
merged PFP layout above, including PRE_ENA, reserved bits 23:22, VMID,
CACHE_POLICY and PRE_RESUME; its header belongs to CE. PAL's public ordinary
builders select neither this form nor the PASID form. These declarations
therefore describe operands without supplying a native submission or address-
space selection protocol for an ordinary compute client.
[CE definition][pal-ce-ib] [Ordinary builder][pal-builder]
[GFX12 ordinary builder][pal12-builder]

Linux's CS parser separately rejects CE submissions unless its explicit
`debug_enable_ce_cs` mode is enabled. A constant-engine opcode declaration
does not make it part of the ordinary compute transport. [CE admission][linux-ib-parser]

## Chained blocks and continuations

PAL places a chaining INDIRECT_BUFFER in a reserved postamble at the end of
its source block. `GfxCmdStream::EndCommandBlock` accounts for that postamble
when computing the aligned block length and inserts any NOP padding before
it. Both the GFX9 and GFX12 command-stream implementations use this
construction. RADV likewise reserves four final DWORDs during finalization;
its chain operation replaces exactly those DWORDs with the next address,
count, CHAIN and VALID fields. [PAL block finalization][pal-block-end]
[GFX9 chunk end][pal-chunk-end] [GFX12 chunk end][pal12-chunk-end]
[RADV finalization][mesa-finalize] [RADV chaining][mesa-chain]

RADV's common tail patcher writes VALID for both graphics and compute streams,
although the cited PFP schemas reserve bit 23. That emitted value and the
source-specific field definition remain separate observations; the patcher
does not establish a universal interpretation of that bit for every engine.
[Tail patcher][mesa-chain] [PFP definition][mesa11-pfp-ib]

```text
source block: commands → alignment NOPs → CHAIN(next address, next DWORD count)
next block:   commands → alignment NOPs → next chain or terminal NOP postamble
```

These builders establish a complete block layout, including the placement of
padding. Appending padding after a chain produces a different stream: CHAIN
replaces the remaining stream, so those trailing NOPs are not its executed
epilogue. The source agreement establishes the callers' construction rule;
it does not specify the hardware response to every other layout.

A chain does not save a return address within its source IB. PAL implements a
compute secondary call by ending the parent block with a chain to the child,
then patching the child's reserved tail to the parent's continuation block.
The continuation's address and complete length become known when that block
is finalized. This path requires exclusive submission of the child because
the caller rewrites its tail. The return is another explicit chain, rather
than an IB2 return stack. [Compute continuation patching][pal-chain-return]
[Pending patch resolution][pal-block-end]

The per-block chain postamble is distinct from a scheduled submission's
postamble stream. PAL's AMDGPU queue chains eligible command buffers but adds
its submission postambles as separate kernel-launched streams. The cited
policy keeps those postambles reachable even when a graphics frame is
preempted; it is not a compute-preemption guarantee. A command owner cannot
silently absorb an external retirement stream into its own chain.
[Scheduled postamble ownership][pal-submit-postamble]

## Publication and memory ownership

Command bytes must be visible before the CP can fetch their reference. KFD's
host producer orders command-memory writes before the write pointer and then
the doorbell. An ACQUIRE_MEM inside the IB cannot publish the bytes that must
already have been fetched to execute it. [Ring publication][kfd-publication]

The KFD IB owner requests uncached executable system storage. In Linux's
GMC11 path, COHERENT or UNCACHED allocation flags become corresponding GEM
flags and select GPU MTYPE_UC. CPU cacheability is a different choice: TTM
selects cached backing unless CPU_GTT_USWC requests write combining, and
cached GTT receives SYSTEM/SNOOPED PTE treatment. This is a specific native
mapping composition, not a property of every host-visible allocation.
[IB allocation][kfd-owner] [Flag realization][linux-flags]
[GPU mapping][linux-mapping] [CPU caching][cpu-cache] [System snooping][system-pte]

The owner retains the full command backing through all published references,
including readable fetch storage outside the useful packet body. A return
establishes architectural continuation; it is not a timestamp for the last
speculative fetch. Shader executable padding has its own
[fetch requirements](dispatch.md#applicability-and-executable-ownership).

A complete raw-MEC lifetime composition is:

```text
write complete command bytes → publish the first-level reference
  → execute IB → CHAIN=0 return to the primary ring
  → join all shader/DMA users and perform required cache work
  → publish and observe a confirmed completion
  → retire the containing command range
  → reuse only storage with no remaining published reference
```

The shader join follows the applicable event/firmware rule in
[memory commands](memory-commands.md#compute-completion-and-firmware).
The KFD caller's in-body join uses a RELEASE_MEM fence in a NOP payload followed
by WAIT_REG_MEM; that body is not immutable. Its placement demonstrates the
separate execution join, not a requirement to store fences in command bytes.
[Body join][kfd-body] [Compute-idle builder][pal-completion]

## Native submission retirement

Windows submission has its own [acceptance, progress-fence and retirement
protocol](../wddm.md). A command's output marker and the native progress fence
have different owners; the node's `RingBufferFenceRelease` capability defines
when and by whom the native fence is updated.

ROCr's `WDDMDevice::SubmitToHwQueue` provides the command address, byte length
and progress point alongside WKMI private submission data. The public WKMI
helper accepts no continuation address and returns no native trailer location.
Those interfaces therefore supply no address to which a caller could chain
to resume an opaque native wrapper. [ROCr hardware-queue submission][rocr-wddm-submit]
[WKMI submission interface][wkmi-submit]

The complete scheduled lifetime needs both the command graph's execution/cache
completion and the selected transport's native retirement. Seeing a payload
marker in the final chained body alone does not establish that the native
submission has released its command backing.

## CPU rebuild after completed use

PAL explicitly permits retained-address reuse. `Reset(false)` retains command
chunks, but the client must first ensure the buffer is neither queued nor
executing and every command buffer that referenced it through nested execution
has also been reset. `Reset` does not perform that wait. The retained chunks
have their recording cursors reset, `GetNextChunk` selects them for subsequent
recording, and finalization copies staged command bytes into mapped GPU storage
when needed. [Reset contract][pal-reset-contract] [Retained chunks][pal-reset]
[Chunk selection][pal-retained] [Finalization][pal-finalize]

The new recording's cache history has a different scope from the lifetime of
those retained chunks. PAL initializes conservative shader/cache state because
other work on the same queue can remain outstanding. Its nested-execution path
also carries the callee's recorded BLT/cache effects into the caller. The
[execution and cache history](cache.md#recorded-execution-and-cache-history)
describes those summaries and the distinct waits and cache actions that clear
them; resetting a summary does not establish storage retirement.

With PAL's automatic reuse and busy tracking enabled, root submit/done counts
and generation tracking determine idleness. The compute postamble waits for
shaders because they may read or write command memory, then increments the
tracker. Its comment relies on a KMD EOP to flush the counter to memory.
That scheduled-submission guarantee is not implicit on a KFD user ring.
When `disableBusyChunkTracking` is set, the client instead guarantees final
GPU use before returning a chunk; the allocator's idle query supplies no
independent completion check. Without automatic reuse, allocator reset owns
recycling. [Allocator modes][pal-allocator-modes]
[Tracker selection][pal-tracker-selection] [Tracker idleness][pal-idle]
[Automatic reuse][pal-reuse] [Compute postamble][pal-postamble]

For a CPU rebuild of a raw first-level IB, the preceding lifetime composition
is therefore a cross-source construction: return to the primary stream,
explicit execution/cache completion, acquired completion, actual primary-ring
retirement, and no other queued reference to the old bytes. Keeping the full
backing mapped through queue removal avoids treating completion as proof of a
precise speculative-fetch endpoint. The CPU then writes and publishes the new
complete body before its next reference. Shader idle alone and ring consumption
alone are insufficient.

Repeated KFD `Submit` calls do not themselves demonstrate same-word rebuild:
its `IndirectBuffer::AddPacket` advances an append cursor. PAL's retained reset
path supplies the distinct rebuild owner evidence. [KFD append behavior][kfd-owner]

## REWIND and command-fetch refresh

REWIND, opcode `0x59`, is two DWORDs with header count 0. PAL describes its
compute use as reloading command-buffer data following the packet. This
fetch-control operation does not generate those bytes, join their producer,
or retire their storage. [REWIND builder][pal-rewind]

| Word | MEC layout | PFP layout |
| --- | --- | --- |
| 0 | Type-3 header, count 0, opcode `0x59`. | Corresponding PFP header. |
| 1 bits 23:0 | Reserved. | Reserved. |
| 1 bit 24 | `offload_enable`. | Reserved. |
| 1 bits 30:25 | Reserved. | Reserved. |
| 1 bit 31 | `valid`. | `valid`. |

These fields are unchanged between PAL's merged and GFX12 definitions. The
PFP representation has no offload operand. PAL's builders construct only the
MEC form; its shader-generation caller supplies OFFLOAD_ENABLE=0 and VALID=1.
Neither an offloaded protocol nor a PFP caller follows from this use.
[Merged MEC][pal-rewind-fields] [Merged PFP][pal-pfp-rewind]
[GFX12 MEC][pal12-rewind-fields] [GFX12 PFP][pal12-pfp-rewind]
[Selected caller][pal-generated]

The emitted pair is `0xc0005902, 0x80000000`: PAL explicitly passes
`ShaderCompute` to its common header helper. Its generated MEC header layout
nevertheless labels bits 7:0 reserved, whereas the common ME/PFP-shaped helper
names shader type at bit 1. This definition/emission difference is retained
as a source-specific value, rather than generalized into a reserved-bit rule
for every MEC packet. [Merged builder][pal-rewind]
[GFX12 builder][pal12-rewind] [MEC header][pal-mec-header]
[Header construction][pal-header-builder]

## GPU-produced command storage

### PAL shader-generated blocks

PAL's `Gfx9::ComputeCmdBuffer::CmdExecuteIndirectCmds` supplies a complete
shader-produced command protocol. The `gfx9` implementation directory also
serves later architectures; it does not identify every PAL compute path.
The caller requires one-time or exclusive submission because execution writes
the generated storage. It consumes borrowed argument/count addresses; an
omitted count address instead uses an embedded DWORD initialized to the
maximum count. [API contract][pal-generation-contract]
[Selected implementation][pal-generated]

The recorder allocates generated chunks from its large embedded-data allocator.
Each chunk reserves fixed-stride commands, alignment padding and a chain tail;
it can also contain per-command user-data spill tables that later shaders
read. The generation dispatch receives the command/spill destinations and
the continuation slots. A generated chunk therefore has more possible readers
than the command parser. [Chunk allocation][pal-generated-allocation]
[Chunk layout][pal-generated-layout] [Generation dispatches][pal-generation-dispatch]

The compute stream then establishes three separate dependencies:

1. `WriteWaitCsIdle` joins the command-producing shader using the applicable
   [compute-completion firmware rule](memory-commands.md#compute-completion-and-firmware).
2. ACQUIRE_MEM with `SyncGlkInv` invalidates the scalar L0 cache. The source
   intends the preceding join to put generated commands in L2; this acquire
   is not an implicit general L2 writeback.
3. REWIND with OFFLOAD_ENABLE=0 and VALID=1 refreshes following command fetch.
   PFP_SYNC_ME cannot supply that step on this MEC stream.

[Complete ordering][pal-generated] [Scalar-cache flag][pal-cache-flags]
[Acquire construction][pal-generation-acquire] [REWIND contract][pal-rewind]

After restoring the application dispatch state, the stream chains through
generated chunks and reserves trailers to return to its continuation.
Finalization attaches generated chunks to the root stream's lifetime tracking.
The final reuse condition covers command fetch and any shaders still using
embedded spill data. REWIND and parsing past the body establish neither that
condition nor host-visible completion. [Chain continuation][pal-generated-chain]
[Generated owners][pal-generated-owner] [Compute postamble][pal-postamble]

### PAL GFX12 indirect execution

The GFX12 compute implementation selects a different mechanism:
`CmdExecuteIndirectCmds` calls `ExecuteIndirectPacket`, which packages borrowed
argument/count addresses, maximum count and byte stride, prepares metadata
and marks the queue-global spill requirement, then calls
`BuildExecuteIndirectV2Ace` to emit the MEC form of EXECUTE_INDIRECT_V2.
That path does not invoke the preceding
shader-generation/REWIND sequence. A GFX12
`BuildRewind` definition therefore cannot establish that this caller uses it.
[GFX12 selection][pal12-generation] [Packet caller][pal12-generation-packet]
[MEC packet builder][pal12-generation-builder]

The [execute-indirect chapter](execute-indirect.md) supplies the complete
base and variable metadata, distinct MEC/PFP packing, dispatch tail, and the
submission-owned allocation/SET_BASE path for global spill. Embedded initial
spill and queue-global spill retain different final readers and owners.

### RADV generated compute continuations

RADV exposes generated commands from GFX8 onward and supplies another concrete
compute continuation protocol. The application supplies token/count inputs and
preprocess storage. A generation shader writes commands, padding, an initial
NOP trailer and a chain from the generated body to that trailer. At a maximum
sequence count of at least 65,536 it selects chains between individual
sequences. That threshold is software policy, not an IB packet count limit.
A generated preamble selects the executed extent when this chaining mode is
active, or when a count address is supplied with a maximum count of at least
64; the latter is RADV's padding-versus-extra-jump heuristic.
[Extension selection][mesa-dgc-selection] [Generated layout][mesa-dgc-layout]
[Chaining and preamble selection][mesa-dgc-policy]
[Generated tail][mesa-dgc-tail] [Trailer initialization][mesa-dgc-trailer]

RADV's preprocess-memory requirements select `memory_types_32bit` and the
maximum graphics/compute IB allocation alignment. Its generator retains the
low 32 bits of the preprocess address and combines them with the device's
`address32_hi` when constructing internal chains. This client therefore needs
its designated address window even though the ordinary packet has a full
high-address DWORD. [Memory requirements][mesa-dgc-memory]
[Allocation alignment][mesa-dgc-alignment] [Generator inputs][mesa-dgc-prepare]
[Generated IB addresses][mesa-dgc-ib]

When the layout does not request EXPLICIT_PREPROCESS, execution records the
generation dispatch and then requests CS_PARTIAL_FLUSH, vector-cache
invalidation and L2 invalidation. Dispatch preparation emits those dependencies
before command execution. Explicit preprocessing has a separate caller-owned
ordering edge; its execution path does not insert this implicit generation
sequence. [Actual execution][mesa-dgc-execute]
[Dispatch preparation][mesa-before-dispatch] [Cache translation][mesa-dgc-cache]

The compute stream uses confirmed WRITE_DATA to replace the generated trailer
with a four-DWORD chain to a new ordinary continuation. The recorder captures
that continuation's address and final DWORD count in the WRITE_DATA payload
before submission. On GFX8 it additionally emits the source's L2-writeback
acquire. It then chains into the generated body:

```text
ordinary stream: join/acquire generator → WRITE_DATA(trailer, CHAIN(continuation))
  → CHAIN(generated body) → generated commands → CHAIN(trailer)
  → patched CHAIN(continuation) → ordinary continuation
```

This MEC path emits no REWIND. Its graphics branch instead uses IB2; neither
branch supplies an ordinary compute return stack. [Compute chain construction][mesa-dgc-chain]
[Write confirmation][mesa-write-confirmation]

The preprocess allocation survives generator writes, ordered trailer patching
and final command fetch. Token/count inputs survive their generator reads;
code, descriptors, spill data and payload survive any subsequently launched
shader uses. Native submission waits/signals and the application lifetime
contract still own final reuse. Stream reset/destruction performs no completion
wait, and a recorded GPU address does not itself retain application-owned
preprocess backing. [Native submission][mesa-native-submit]
[Stream reset][mesa-reset] [Stream destruction][mesa-destroy]

[kfd-caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/KFDTestUtil.cpp#L283-L323
[kfd-submit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/Dispatch.cpp#L82-L104
[kfd-body]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/Dispatch.cpp#L265-L276
[kfd-packet]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/PM4Packet.cpp#L302-L323
[kfd-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/include/pm4_pkt_struct_common.h#L211-L256
[pal-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1571-L1632
[pal-compute-chain]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdStream.cpp#L466-L546
[pal-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L2670-L2716
[pal-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L805-L810
[pal-reset-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2244-L2306
[mesa-secondary]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L697-L756
[kfd-publication]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/PM4Queue.cpp#L57-L76
[kfd-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/IndirectBuffer.cpp#L30-L53
[linux-flags]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L1762-L1787
[linux-mapping]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c#L492-L525
[cpu-cache]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1190-L1214
[system-pte]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1432-L1476
[pal-completion]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4284-L4337
[pal-reset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L482-L525
[pal-retained]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStream.cpp#L365-L395
[pal-finalize]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L347-L389
[pal-idle]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L470-L479
[pal-reuse]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L703-L739
[pal-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1230-L1267
[pal-generated]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1444-L1543
[pal-rewind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3541-L3561
[pal-generated-chain]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdStream.cpp#L571-L606
[pal-generated-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L236-L262
[pal-block-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdStream.cpp#L115-L205
[pal-chunk-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdStream.cpp#L1525-L1547
[pal12-chunk-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdStream.cpp#L68-L94
[mesa-finalize]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L478-L510
[mesa-chain]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L560-L592
[pal-chain-return]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdStream.cpp#L519-L549
[pal-submit-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuQueue.cpp#L1235-L1313
[rocr-wddm-submit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/device.cpp#L1134-L1164
[wkmi-submit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/shared/amdgpu-windows-interop/wkmi/wkmi.h#L299-L312
[pal12-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1588-L1646
[pal-pfp-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L2536-L2593
[pal12-pfp-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L2621-L2678
[pal12-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1389-L1432
[pal-pasid]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1631-L1702
[pal12-pasid]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1648-L1719
[pal-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_pm4_it_opcodes.h#L35-L110
[pal-ce-ib]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_ce_pm4_packets.h#L671-L728
[pal-rewind-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L2076-L2101
[pal-pfp-rewind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L3323-L3346
[pal12-rewind-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L2144-L2169
[pal12-pfp-rewind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L3773-L3796
[pal12-rewind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2552-L2571
[pal-mec-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L44-L54
[pal-header-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L274-L302
[pal-generation-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4564-L4591
[pal-generated-allocation]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L2311-L2361
[pal-generated-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1548-L1617
[pal-generation-dispatch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/gfx9/gfx9RsrcProcMgr.cpp#L4385-L4692
[pal-cache-flags]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Chip.h#L627-L650
[pal-generation-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L614-L717
[pal12-generation]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L854-L863
[pal12-generation-packet]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L771-L852
[rocr-ib-macros]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_gpu_pm4.h#L48-L75
[kfd-policy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/include/pm4_pkt_struct_common.h#L69-L72
[kfd-header]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/PM4Packet.cpp#L37-L44
[mesa11-mec-ib]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L13972-L14066
[mesa11-pfp-ib]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L3885-L3979
[mesa12-mec-ib]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L14202-L14296
[mesa12-pfp-ib]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L3982-L4076
[mesa-userq-selection]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1493-L1507
[mesa-userq-ib]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L1488-L1621
[linux-ib-properties]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L438-L481
[linux-ib-parser]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L348-L416
[linux-ib-capacity]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L45-L63
[mesa-ib-padding]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L464-L475
[mesa-ib-alignment]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L478-L534
[linux-ib-padding]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L146-L189
[linux11-rings]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L7070-L7194
[linux121-rings]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L4198-L4253
[linux6-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v6_0.c#L1929-L1952
[linux7-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v7_0.c#L2222-L2254
[linux8-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c#L6012-L6044
[linux9-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c#L5551-L5584
[linux943-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L2950-L2983
[linux10-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c#L8686-L8719
[linux11-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L6041-L6074
[linux12-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c#L4546-L4563
[linux121-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L3704-L3736
[pal-native-ib]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuQueue.cpp#L1911-L1984
[mesa-dgc-selection]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L846-L857
[mesa-dgc-layout]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_dgc.c#L23-L69
[mesa-dgc-policy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_dgc.c#L115-L139
[mesa-dgc-tail]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_dgc.c#L1095-L1158
[mesa-dgc-trailer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_dgc.c#L1160-L1209
[mesa-dgc-execute]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L14844-L15014
[mesa-before-dispatch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15320-L15358
[mesa-dgc-cache]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L59-L528
[mesa-dgc-chain]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L759-L834
[mesa-write-confirmation]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L56-L80
[mesa-native-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1718-L1883
[mesa-reset]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L512-L547
[mesa-destroy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L174-L190
[mesa-dgc-memory]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_dgc.c#L3066-L3083
[mesa-dgc-prepare]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_dgc.c#L3279-L3394
[mesa-dgc-ib]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_dgc.c#L1029-L1043
[pal-allocator-modes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdAllocator.h#L43-L70
[pal-tracker-selection]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L93-L118
[mesa-dgc-alignment]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_dgc.c#L71-L77
[pal12-generation-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L3192-L3292
