# Inbound remote-write visibility

An external DMA producer can write GPU-owned memory while the host coordinates
when a GPU may consume it. The receive path combines the producer's completion
protocol, visibility at the target device, and the consuming GPU's execution
and cache ordering. A visibility operation at the target does not itself join
the producer or schedule the consumer.

HIP's `hipDeviceFlushGPUDirectRDMAWrites` supplies a host-ordered visibility
barrier for inbound writes from an external device such as a NIC. Its contract
explicitly excludes synchronization with streams and kernels. The public
implementation examined here is ROCm systems
`105dd4ff35798f95646353bc08f6c885416ae17e`; its native register-discovery path
below uses Linux KFD. The HIP API contract, runtime policy and native mapping
have different applicability. [HIP contract][hip-contract]

## Targets, scopes and support queries

The API accepts the current HIP device as its target and one of two observer
scopes. These are inbound-write visibility scopes, separate from HSA packet
fence scopes and from the addressability of a peer mapping.
[Target and scope definitions][hip-scopes]

| API field or query | Representation and meaning at the cited revision |
| --- | --- |
| `hipFlushGPUDirectRDMAWritesTargetCurrentDevice` | Target value 0; the device whose memory receives the remote writes. |
| `hipFlushGPUDirectRDMAWritesToOwner` | Scope value 100; visibility to the memory's owning device. |
| `hipFlushGPUDirectRDMAWritesToAllDevices` | Scope value 200; the API promises visibility to all HIP devices. It does not grant those devices access to an allocation. |
| `hipDeviceAttributeGPUDirectRDMASupported` | HIP reports 1 when both `dmabufSupported_` and `largeBar_` are true. This predicate supplies neither NIC registration nor a particular peer route. |
| `hipDeviceAttributeGPUDirectRDMAFlushWritesOptions` | `Host` is bit 0 and `MemOps` is bit 1. This implementation reports only `Host`, and only when the HDP memory-flush pointer is non-null. |
| `hipDeviceAttributeGPUDirectRDMAWritesOrdering` | The enum assigns None=0, Owner=100 and AllDevices=200. This implementation always reports None. |

[Definitions][hip-scopes] · [Property construction][hip-properties] ·
[Attribute queries][hip-attributes] · [Flush and ordering policy][hip-policy]

General RDMA support and host-flush availability are independent predicates.
The flush entry point tests the `Host` bit; it does not test the general RDMA
support property, identify an allocation, or inspect a producer's transport.
Likewise, the declared `MemOps` bit is not an advertised device-queue flush
facility in this implementation. [Flush implementation][hip-flush]

The API describes a no-op when the reported natural ordering scope already
covers the requested scope. At this source revision, the helper always returns
None, so neither accepted scope takes that branch. The implementation checks
host-flush support before the ordering comparison. [HIP contract][hip-contract]
[Implementation order][hip-flush]

## Native register mapping and lifetime

The HDP pointer is discovered through the runtime's device connection. CLR
queries `HSA_AMD_AGENT_INFO_HDP_FLUSH`; ROCr fills the result from the agent's
`HSA_HEAPTYPE_MMIO_REMAP` memory bank. Absent that bank, the two pointers remain
null. The HSA extension explicitly places direct use of these registers
outside the HSA memory model. A register pointer therefore does not carry the
release/acquire contract of an HSA signal. [CLR query][clr-query]
[ROCr discovery][rocr-discovery] [Null initialization][rocr-default]
[HSA extension][hsa-extension]

The Linux UAPI defines byte offsets within the remapped page:

| Register | Byte offset | Role in this HIP call |
| --- | --- | --- |
| `KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL` | 0 | A 32-bit host write followed by a 32-bit read. |
| `KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL` | 4 | Discovered alongside the first pointer; not accessed by this call. |

[Remap ABI][remap-abi] · [ROCr pointer construction][rocr-discovery] ·
[HIP operation][hip-flush]

The thunk allocates one page with `MMIO_REMAP | WRITABLE | COHERENT`, maps it
for CPU access through the KFD file, and also creates a GPU mapping. These
attributes describe the register mapping, not the payload allocation. KFD's
CPU mapping is noncached and uses `VM_IO`, `VM_DONTCOPY`, `VM_DONTEXPAND`,
`VM_NORESERVE`, `VM_DONTDUMP` and `VM_PFNMAP`. The allocation and mmap paths
reject host page sizes above 4096 bytes; allocation also requires a nonzero
native remap bus address. [Thunk mapping][thunk-map]
[KFD allocation][kfd-allocation] [CPU mapping][kfd-mapping]

Remap availability depends on native IP and operating mode. For example,
Linux's NBIO 7.0, 7.9 and 7.11 setup exposes the remap bus address only outside
SR-IOV VF mode and with a page size no greater than 4096. Their fallback retains
a kernel register offset while clearing the userspace remap bus address.
Kernel access to HDP therefore does not imply availability of the HSA pointer.
[NBIO 7.0][nbio-70] [NBIO 7.9][nbio-79] [NBIO 7.11][nbio-711]

The thunk attempts these mappings for nodes selected by its SVM-aperture
predicate: a system treated as discrete-GPU, or an engine version at least
`GFX_VERSION_VEGA10`. Topology exposes the MMIO bank only after the aperture
was successfully mapped. These are discovery predicates; compiler target
names alone do not establish host-flush availability. [Node predicate][thunk-predicate]
[Mapping attempt][thunk-create] [Topology publication][thunk-topology]

The thunk's process-aperture teardown unmaps the GPU view, unmaps the CPU page
and releases the allocation. The HDP pointer is borrowed from that native
lifetime; freeing or exporting a payload allocation neither creates nor owns
it. Payload backing, producer registration and peer mappings retain their own
lifetimes. [Register teardown][thunk-release]
[Shared-memory ownership](external-memory.md)

## Host operation and return

After initialization and argument validation, the HIP implementation:

1. Selects the current device's HDP memory-flush pointer.
2. Rejects an unavailable host-flush option with `hipErrorNotSupported`.
3. Compares the reported natural ordering scope with the requested scope.
4. Writes 1 through a volatile 32-bit pointer, reads the same register once,
   discards the read value and returns success.

Invalid targets or scopes return `hipErrorInvalidValue`. The successful path
is identical for Owner and AllDevices. It contains no completion-bit polling,
queue packet, stream join, peer enumeration or NIC completion query.
[Complete implementation][hip-flush]

This establishes the runtime's concrete mechanism, while the API declaration
supplies its claimed visibility guarantee. It is not a published per-IP proof
of that guarantee. The read value is not interpreted as a hardware status or
an error report. Linux's generic kernel HDP path also uses a remapped register,
but writes 0 and performs an NBIO memory-size read when available; NBIO 7.9's
callback reads `RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE`. Those are distinct sequences,
not evidence that arbitrary raw-register variants are interchangeable.
[Kernel flush][linux-flush] [Readback callback][linux-readback]

HDP and shader caches remain separate parts of the memory path. Linux's
host-aperture handling and the shader/DMA recipes distinguish HDP maintenance
from GL2 and shader-cache release/acquire. The HIP function neither emits a
consumer cache operation nor inserts synchronization into an executing kernel.
Its AllDevices scope does not specify the cache state or access permissions
of each possible peer consumer. [HDP boundary](../gpu/recipes/local-memory.md#hdp-and-the-host-aperture-boundary)
[Shader memory ordering](../gpu/shader-memory.md)

## Receive sequence and resource reuse

Consider an external producer writing allocation `P` owned by GPU A, then a
new GPU dispatch reading `P`. The producer's registration covers the intended
range, the producer has a valid native route to it, and every consuming GPU
has its own valid mapping. Earlier users of this version of `P` have finished
before the producer overwrites it. Those premises come from the memory and
transport owners, not from the flush API.

| Actor | Operation and ownership edge |
| --- | --- |
| External producer | Write `P` and provide the transport's completion or ordering guarantee for the intended inbound writes. Submission acceptance alone does not establish that point. |
| Host | Observe that producer guarantee using its documented completion protocol; select GPU A as the current target and perform the supported visibility operation at the required observer scope. |
| Host or queue owner | Make consuming work eligible only after that operation returns. A prequeued consumer needs an explicit dependency released at this point. |
| GPU consumer | After the dependency, acquire the payload through the actual mapping/cache path, then read `P`. AQL packet scopes and PM4 cache operations belong to their respective queue contracts. |
| Reuse owner | Observe completion of every reader and every remaining producer access before overwriting `P`, unregistering it, changing mappings or freeing backing. |

This is a composition of contracts, not an extra guarantee supplied by HIP.
The flush takes no producer completion object, memory range or consumer queue,
so it cannot infer these ownership edges. A control word used to release a
consumer needs its own coherent placement, publication and stable generation.
[HIP boundary][hip-contract] [Execution dependencies](pipelines.md)
[AQL fence scope](../gpu/aql/barriers.md#fence-scope-and-observers)
[PM4 cache transitions](../gpu/pm4/cache.md)

For GPU B reading GPU A's allocation, peer mapping and the directed cache
route remain explicit. A host request for AllDevices supplies the HIP scope;
it does not transform an owner-only allocation into peer-visible memory or
replace GPU B's consuming synchronization. The implementation's single local
HDP operation alone is insufficient evidence to derive a native recipe for
every peer topology. That derivation needs the target IP's HDP completion and
remote-cache contracts in addition to the source sequence above.
[Peer memory routes](../gpu/recipes/host-device.md#peer-gpu-handoff)

## Resident consumers and host-free handoffs

A resident kernel's entry acquire occurs once. Remote updates arriving later
need a per-generation dependency, fresh observation of the control value and
payload acquisition at each consumption point. Calling the host flush while
that kernel is running does not make it pause, discard prefetched payload or
re-execute its entry acquire. [Resident publication](../gpu/shader-memory.md#global-release-and-acquire-sequences)
[Generation ownership](pipelines.md)

This HIP implementation is host-mediated and advertises no MemOps flush
option. A device-generated SDMA or GPU/NPU protocol therefore needs its own
producer completion, visibility and consumer-acquisition sequence. The
existence of the host API, or of a GPU mapping of its register page, does not
establish such a protocol. [Device-generated SDMA](../gpu/sdma/device-publication.md)
[GPU/array handoff](../gpu/recipes/gpu-npu.md)

Runtime stream publication is a separate mechanism. CLR uses queue completion
signals when handing earlier work to native SVM operations and SYSTEM-scoped
dispatches before publishing batch control values. Those operations carry
specific stream dependencies that the host RDMA flush does not receive.
[Prior-stream publication](../gpu/recipes/host-device.md#publishing-prior-stream-work-to-another-executor)

[hip-contract]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/hip/include/hip/hip_runtime_api.h#L2534-L2556
[hip-scopes]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/hip/include/hip/hip_runtime_api.h#L678-L706
[hip-properties]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_device.cpp#L808-L812
[hip-attributes]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_device_runtime.cpp#L453-L461
[hip-policy]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_internal.hpp#L766-L776
[hip-flush]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_device_runtime.cpp#L519-L548
[clr-query]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L662-L671
[rocr-discovery]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L584-L588
[rocr-default]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_gpu_agent.h#L771-L772
[hsa-extension]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L787-L792
[remap-abi]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/include/uapi/linux/kfd_ioctl.h#L744-L747
[thunk-map]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/fmm.c#L3547-L3600
[kfd-allocation]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L1171-L1220
[kfd-mapping]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L3692-L3724
[nbio-70]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdgpu/nbio_v7_0.c#L288-L300
[nbio-79]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdgpu/nbio_v7_9.c#L469-L484
[nbio-711]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdgpu/nbio_v7_11.c#L364-L376
[thunk-predicate]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/topology.c#L869-L885
[thunk-create]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/fmm.c#L4054-L4068
[thunk-topology]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/topology.c#L2483-L2490
[thunk-release]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/fmm.c#L3602-L3614
[linux-flush]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdgpu/amdgpu_hdp.c#L51-L69
[linux-readback]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdgpu/nbio_v7_9.c#L70-L73
