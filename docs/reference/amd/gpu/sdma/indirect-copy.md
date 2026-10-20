# SDMA indirect source and destination copies

An indirect copy lets SDMA read a payload address from memory when the copy
executes. A producer can choose the source, destination, or both after command
construction, while the command retains a fixed byte length. This separates
address selection from command publication: the producer updates an address
slot and satisfies a dependency; SDMA dereferences the slot and transfers the
payload. ROCr calls these operations `LINEAR_INDIRECT_SRC`,
`LINEAR_INDIRECT_DST`, and `LINEAR_INDIRECT_SRCDST`. [Operation contract][api]

This is payload-address indirection. An [indirect command buffer](command-buffers.md)
instead supplies commands to the engine. [Device-generated commands](device-publication.md)
can also change lengths and command sequences, with their own reservation,
publication, and retirement obligations.

## Applicability

The cited ROCr layout is
`SDMA_PKT_COPY_LINEAR_WAITSIGNAL_INDIRECT_GFX1250`. `BlitSdma::Initialize`
enables its indirect-copy path when ISA major is 12 and minor is at least 5.
HIP/CLR's `sdma_indirect_supported_` setting uses the same ISA condition.
These are consumer predicates at the cited revisions, not a native-SDMA-IP
feature query or a promise for every numerically later target.
[ROCr selection][initialize] [CLR selection][settings]

The public HIP flags are `hipMemcpyFlagExtOpIndirectSrc` (`0x400`) and
`hipMemcpyFlagExtOpIndirectDst` (`0x800`); their combination selects both sides.
HIP rejects combining either with the swap flag. Its batch path further
restricts admission by the memory objects naming the operands, as described
below. A packet definition alone does not establish support through another
native queue or Windows submission interface. [Flags][hip-flags]
[Flag validation][hip-front-door] [Operand admission][hip-admission]

## Operand modes and representation

Let `S` and `D` be the two command operands, `load_pointer(P)` read a device
virtual address from slot `P`, and `L` be the positive command-carried byte
length. Each indirect operand names one slot, with one level of dereference.
The ranges below have exclusive end addresses and contain exactly `L` bytes.

| Mode | `indirect_src` | `indirect_dst` | Source range | Destination range |
| --- | --- | --- | --- | --- |
| `LINEAR_INDIRECT_SRC` | 1 | 0 | `load_pointer(S) .. + L` | `D .. + L` |
| `LINEAR_INDIRECT_DST` | 0 | 1 | `S .. + L` | `load_pointer(D) .. + L` |
| `LINEAR_INDIRECT_SRCDST` | 1 | 1 | `load_pointer(S) .. + L` | `load_pointer(D) .. + L` |

The builder passes slot addresses unchanged into the source/destination
fields. The host caller does not dereference those slots. Multi-entry operations
share one mode; each entry has its own operands and byte length.
[Descriptor][api] [Scalar and multi-entry caller][indirect-caller]
[Packet builder][builder]

### Header and optional blocks

Opcode `SDMA_OP_COPY` is 1. `SDMA_SUBOP_COPY_INDIRECT` aliases
`SDMA_SUBOP_COPY_LINEAR`, whose value is 0; the indirection bits distinguish
the form. The header occupies DWORD 0:

| Bits | Native field | ROCr emission |
| --- | --- | --- |
| 7:0 | `op` | 1. |
| 15:8 | `sub_op` | 0. |
| 18 | `tmz` | Zero. |
| 20 | `indirect_src` | Selected by the operation mode. |
| 21 | `indirect_dst` | Selected by the operation mode. |
| 28 | `npd` | Zero. |
| 30 | `wait` | One when a dependency signal is supplied to this packet. |
| 31 | `signal` | One when a completion signal is supplied to this packet. |
| 17:16, 19, 27:22, 29 | Reserved | Zero. |

The serialized order is header, optional seven-DWORD WAIT block, six-DWORD
copy block, optional five-DWORD SIGNAL block. The C structure describes the
maximum form; absent blocks consume no command bytes.
[Opcode definitions][constants] [Header layout][layout] [Serialization][builder]

| WAIT | SIGNAL | Copy block begins at DWORD | SIGNAL begins at DWORD | Total DWORDs / bytes |
| --- | --- | --- | --- | --- |
| 0 | 0 | 1 | Absent | 7 / 28 |
| 1 | 0 | 8 | Absent | 14 / 56 |
| 0 | 1 | 1 | 7 | 12 / 48 |
| 1 | 1 | 8 | 14 | 19 / 76 |

### Copy block

Offsets in this table are relative to the start of the six-DWORD copy block.
Source and destination address pairs are low word followed by high word.

| Relative DWORD | Bits | Native field and meaning |
| --- | --- | --- |
| 0 | 29:0 | `copy_count`: byte length minus one. Bits 31:30 are reserved. |
| 1 | 11:10 | `indirect_addr_scope`: scope of the address-slot reads. |
| 1 | 14:12 | `indirect_temporal_hint`: policy hint for address-slot reads. |
| 1 | 19:18 | `copy_dst_scope`: scope of payload writes. |
| 1 | 22:20 | `copy_dst_temporal_hint`: destination policy hint. |
| 1 | 27:26 | `copy_src_scope`: scope of payload reads. |
| 1 | 30:28 | `copy_src_temporal_hint`: source policy hint. |
| 1 | 9:0, 17:15, 25:23, 31 | Reserved. |
| 2–3 | All | `copy_src_addr_31_0`, `copy_src_addr_63_32`: full byte address of the source or its address slot. |
| 4–5 | All | `copy_dst_addr_31_0`, `copy_dst_addr_63_32`: full byte address of the destination or its address slot. |

ROCr emits SYS (3) for all three scopes and zero for the temporal hints.
Address-slot visibility and payload visibility remain distinct even though
this builder selects the same scope. SYS is an observer scope, not a choice
of physical memory placement. [Copy layout][copy-layout] [Builder][builder]
[Scope constants][constants]

### Wait and signal blocks

The WAIT block contains a function/policy word, 64-bit address, 64-bit
reference, and 64-bit mask. Its function occupies bits 2:0, scope 19:18,
and temporal hint 22:20 of its first word. The builder emits EQ (3), SYS,
reference zero, and an all-ones mask. The SIGNAL block contains an
operation/policy word, 64-bit address, and 64-bit data. Its operation occupies
bits 6:0, scope 19:18, and temporal hint 22:20. ROCr emits SUB64 (`0x70`),
SYS, and operand one. Other policy bits remain zero. [Layouts][layout]
[Operands][builder]

WAIT and SIGNAL address words reserve their low three bits, expressing
eight-byte alignment. The copy operand fields carry all address bits. That
representation does not establish the hardware's alignment or atomic-read
guarantee for an address slot. HIP's validation checks `sizeof(void*)` bytes
on an indirect side; neither that range check nor a 64-bit address field
proves a concurrent slot update is safe. A stable, naturally aligned native
pointer published before its dependency is satisfied supplies the slot used
in the programming sequence below. [Fields][layout]
[Pointer-holder range checks][hip-validation]

## Lengths, chunking, and operation arrays

The 30-bit count represents lengths from one byte through `0x40000000`
bytes. ROCr's normal major-12 factory instead selects a per-packet cap of
`0x3fffffff` bytes; disabling its copy-size override selects the older
`0x3fffe0` cap. The [ordinary-copy chapter](copy.md#runtime-caps-and-policy-margins)
distinguishes these runtime limits from representable lengths.
[Factory][factory] [Fallback cap][fallback-cap]

`DmaCopyFanOutOp` rejects an indirect entry exceeding its selected
`MaxSingleLinearCopySize`. The representation has no post-dereference offset:
advancing a slot address would read another slot, rather than advance the
payload address stored in the first one. Direct-copy chunking therefore does
not implement indirect chunking. Several independently prepared slots can
describe successive valid ranges, but the cited runtime does not manufacture
those slots. [Length admission][indirect-admission] [Fields][copy-layout]

The HSA descriptor uses `num_entries == 0` for a scalar operation. Its
multi-entry form accepts 1–1024 entries, with `src_list`, `dst_list`,
`dst_agent_list`, and `size_list`, plus one common source agent. These CPU
arrays describe device operands; they are not themselves the slots SDMA
dereferences. `DmaCopyIndirect` folds the scalar form into the same owner.
[Array contract][api] [Validation][hsa-validation]
[Common caller][indirect-caller]

CLR buckets indirect operations by direction and mode, then supplies one
multi-entry descriptor for each bucket. Its `uint16_t` count conversion and
single-bucket construction do not split an arbitrarily large HIP batch into
the HSA API's 1024-entry groups. The HSA limit is a software operation-array
limit, separate from one packet's fixed two operands. [Buckets][clr-buckets]
[Descriptor construction][clr-submit] [HSA limit][hsa-validation]

## Executor selection and HIP ownership

The HIP batch caller resolves the allocations containing `S` and `D` before
submission. For an indirect operand, that is the address-slot allocation;
the future pointee need not be known to the host. It validates the slot-sized
range, and the full `L`-byte range on a direct side, using those memory
objects' access and bounds checks. [Validation][hip-validation]
[Access and bounds][hip-bounds] [Two-size helper][hip-helper]

The extended-operation admission accepts device/host combinations, ordinary
device-to-device copies and peer copies as classified from those operand
objects. `getMemoryType` classifies an object as host when its flags include
`CL_MEM_SVM_FINE_GRAIN_BUFFER` or `CL_MEM_USE_HOST_PTR`, and as device otherwise.
These are runtime object categories, not a test of the future pointee's
physical placement. The admission rejects host-to-host copies and
pageable-host read/write paths.
[Memory classification][hip-memory-type] [Admission][hip-admission]

Source/destination location hints are present in the public attributes, but
this caller selects queues from memory objects and device IDs. Later pointer
contents do not repeat that selection. [Attributes][hip-flags]
[Classification and queues][hip-admission]

For the admitted device/host paths, HIP chooses the device-side allocation's
device; for ordinary device-to-device copies it chooses the destination
allocation's device, and for peer copies it chooses the stream's device.
CLR obtains the operands' device addresses and owning agents, normalizes peer
routing to the calling device, and groups operations into H2D, D2H, or D2D bins. ROCr then
selects copy engines using the supplied agents and topology. These operations
precede SDMA's slot reads. [HIP queues][hip-admission]
[CLR operands and routing][clr-operands] [Direction bins][clr-bins]
[Engine assignment][engine-owner]

Consequently, late address selection is bounded by already established
mapping and engine reach. Keeping a slot accessible does not map its pointee,
select a different executor, or acquire ownership of the pointee allocation.
The producer chooses payload ranges with valid mappings for the selected
engine and keeps those mappings live through their final users. This follows
from the information passed across the caller chain; it is not a hardware
allocation service.

`BatchCopyMemoryCommand` owns its copied/moved vector of descriptors, whose
memory fields are pointers to the operand objects. CLR's temporary HSA arrays
remain stable during synchronous packet construction. The command has no
separate object reference obtained by reading an indirect slot. Its host-cache
sync and destination-write bookkeeping operate on the operand objects too.
Those mechanisms cannot establish visibility or retirement for an otherwise
unknown pointee. [Command representation][command]
[Array storage][clr-buckets] [Submission and joins][clr-submit]
[Memory bookkeeping][clr-join]

The `srcAccessOrder` field distinguishes source consumption from command
completion. The [source-lifetime comparison](../recipes/host-device.md#source-consumption-and-copy-completion)
details the public header disagreement and the pageable `DuringApiCall`
snapshot paths. Indirect copies instead take `BatchCopyMemoryCommand`, carrying
the slot's address for later device access. Capturing pageable input therefore
does not establish that an indirect slot or its payload can be reused at API
return. [Attribute definitions][hip-flags] [Command creation][hip-enqueue]

When HIP routes work onto another device's queue, it carries the original
stream's preceding command as a dependency and adds a joining marker back to
the original stream. The returned stream order therefore includes those
other queues; queue selection alone would not provide that relationship.
[Cross-queue dependencies][hip-enqueue] [Stream join][hip-stream-join]

## Dependencies, completion, and reuse

For each nonempty engine group, ROCr attaches the first dependency to the
first copy's WAIT block and completion to the last copy's SIGNAL block.
Additional dependencies use preceding `POLL_MEM_64B` packets. Intermediate
copies have neither optional block. Profiling inserts a coordinator timestamp
and start signal; without profiling, the bodies consume the original
dependencies directly. [Dependency selection][indirect-admission]
[Body serialization][bodies] [Coordinator][coordinator]

Each group's last packet subtracts one from the operation's output signal.
The [fan-out completion protocol](fanout.md#completion-ownership) accounts
for those credits and any coordinator release, timestamp, or notification
tail. CLR requests a separate `ActiveSignal(1, ...)` for each merged operation
and joins independent completions into the enclosing command. That actual
caller differs from the public batch description's shared-count model.
The [batch boundary](fanout.md#batch-composition-and-descriptor-ownership)
compares all operation families' count units and descriptor propagation.
[CLR signals][clr-submit] [Enclosing join][clr-join]
[API description][batch-api]

The public descriptor also advertises a raw wait, raw signal, and
`wait.scope` for indirection reads. `DmaCopyIndirect` forwards the HSA
dependencies, completion signal, mode, operands, agents, and lengths; it does
not forward those raw fields. Its builder hardcodes the SYS/EQ-zero/SUB64
operands above. The descriptor therefore supplies no evidence of configurable
raw comparisons or pointer-read scope on this path.
[Descriptor][api] [Caller][indirect-caller] [Builder][builder]

A complete successful producer/copy/consumer flow is:

1. The owner establishes native mappings for commands, address slots, control
   cells, source ranges, and destination ranges. Each chosen engine can reach
   its operands and their eventual pointees. Source and destination payloads
   have independent, nonoverlapping ranges.
2. The producer prepares the source payload and writes each selected address
   into its slot. It completes those writes and publishes them through the
   [cache and visibility protocol](cache.md) before satisfying the copy's
   dependency. Slots remain stable through their last SDMA reader.
3. Commands already published through the native [queue protocol](publication.md)
   wait for that dependency, read the selected addresses, and copy the fixed
   byte ranges. Slot and payload mappings remain live throughout this work.
4. The downstream consumer waits for all contributing copies and acquires the
   destination through its own access path. Neither API return nor ring-byte
   consumption establishes payload completion.
5. Reuse follows the final user of each resource. Copy completion is a useful
   conservative reuse boundary for slots and sources; destinations remain live
   through downstream readers. Completion and notification cells remain live
   through their final writer, waiter, and any native notification tail.

Through HIP, this flow uses stream-ordered source access and explicit stream
dependencies for producers on other streams. Copies within one batch have no
specified order relative to one another; array order does not express a
producer/consumer edge. [Attribute contract][hip-flags]
[Batch ordering contract][command]

For example, an expert-selection shader can choose a mapped source region and
publish its address for a fixed-size fetch into a preselected destination.
The byte length and SDMA route stay fixed; the shader chooses the data. A
producer selecting a new length as well uses a different mechanism, such as
device-generated commands. The sequence removes a host decision between
selection and transfer, but does not establish a latency or bandwidth result.

## Software-path distinctions

At the cited CLR revision, non-linear operations initially take the SDMA
path. Its failure branch excludes swap from shader fallback, but does not
exclude indirect modes. `ShaderCopyBufferBatchRaw` builds direct-address
descriptors without consuming the indirection mode. That fallback is not an
equivalent implementation of indirect copy. Separately, fan-out's incremental
publication gives an error return no general cancellation guarantee for
already published work. The successful lifetime sequence above does not
establish error recovery for either boundary.
[Path selection and fallback][clr-fallback] [Shader descriptors][clr-shader]
[Fan-out publication and cleanup][publication-owner]

RADV's Vulkan indirect-memory-copy implementation illustrates another
meaning of “indirect copy.” A compute shader reads transfer descriptors;
preprocessing derives indirect dispatch dimensions, then the copy shader
loads the source, destination, and size from memory. It runs on shader
execution resources and does not emit the SDMA packet described here.
It is independent evidence for an indirect-copy software mechanism, not
corroboration of this SDMA layout. [RADV dispatch][mesa-dispatch]
[RADV shaders][mesa-shaders]

[api]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2311-L2394
[initialize]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L156-L218
[settings]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocsettings.cpp#L198-L201
[hip-flags]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/hip/include/hip/driver_types.h#L455-L493
[hip-front-door]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_memory.cpp#L3245-L3314
[hip-admission]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_memory.cpp#L3085-L3169
[indirect-caller]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2060-L2085
[constants]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L55-L81
[layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1432-L1600
[copy-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L1508-L1558
[builder]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2800-L2866
[factory]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L856-L906
[fallback-cap]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L142-L150
[indirect-admission]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1683-L1741
[hsa-validation]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L640-L669
[clr-buckets]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L785-L861
[clr-submit]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L931-L1039
[hip-validation]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_memory.cpp#L3021-L3081
[hip-bounds]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_memory.cpp#L521-L539
[hip-memory-type]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_memory.cpp#L122-L130
[hip-helper]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_internal.hpp#L835-L848
[clr-operands]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L647-L717
[clr-bins]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L720-L761
[engine-owner]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1547-L1665
[command]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/platform/command.hpp#L1181-L1272
[clr-join]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L3726-L3792
[hip-enqueue]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_memory.cpp#L2959-L3009
[hip-stream-join]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/hipamd/src/hip_memory.cpp#L3198-L3242
[bodies]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1351-L1490
[coordinator]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1078-L1348
[batch-api]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2396-L2429
[clr-fallback]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L3172-L3255
[clr-shader]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocblit.cpp#L2798-L2908
[publication-owner]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1757-L1902
[mesa-dispatch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy_indirect_cs.c#L139-L190
[mesa-shaders]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/nir/radv_meta_nir.c#L1390-L1462
