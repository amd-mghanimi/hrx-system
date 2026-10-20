# Cache residency and replacement policy

Shader temporal hints describe expected reuse and influence which cache lines
are retained under pressure. They operate on memory accesses; they do not
reserve a byte range or establish a producer/consumer dependency. RDNA4 and
CDNA5 define the hint separately from the access's coherence scope and the
writeback/invalidate operations. [RDNA4 §4.1.1][rdna4-policy]
[CDNA5 §4.1.1][cdna5-policy]

A persisting-cache size request is a different native control. Its usefulness
depends on the driver that admits the request and owns the hardware setting.
This chapter separates the ISA policy, ROCr's request path, and HIP's property
and access-window interfaces. Native support is attributed to the particular
driver revision and handler that supplies it.

## RDNA4 and CDNA5 temporal hints

The following VMEM `TH[2:0]` encodings agree between RDNA4 Tables 13–14 and
CDNA5 Tables 10–11. `RT`, `NT` and `HT` denote ordinary, low-reuse and
high-reuse priority, respectively; `HT` has priority over `RT`.

| Value | Loads | Stores |
| --- | --- | --- |
| `0` | `RT` at near and far caches. | `RT` at both. |
| `1` | `NT` at both. | `NT` at both. |
| `2` | `HT` at both. | `HT` at both. |
| `3` | `LU`: last use; a hit can discard dirty data. | `WB`: high priority plus far-cache dirty retention. |
| `4` | Near `NT`, far `RT`. | Same. |
| `5` | Near `RT`, far `NT`. | Same. |
| `6` | Near `NT`, far `HT`. | Same. |
| `7` | Reserved. | Near `NT`, far `WB`. |

[RDNA4 encodings][rdna4-policy] · [CDNA5 encodings][cdna5-policy]

Atomic instructions interpret `TH` differently: bit 0 selects a returned
value, bit 1 selects non-temporal policy, and bit 2 selects deferred scope
for non-returning operations. Thus the load/store value `2` is not an atomic
high-priority hint. [RDNA4 Table 15][rdna4-policy]
[CDNA5 Table 12][cdna5-policy]

These fields supply no guaranteed resident capacity or cache-line lifetime.
The [shader publication rules](shader-memory.md) still govern producer
release, fresh control observation, consumer acquire and completion of prior
reads. In particular, changing replacement priority cannot replace the
cache-maintenance operation required by a directed memory handoff.

## ROCr's persisting-size interface

ROCm systems revision `105dd4ff35798f95646353bc08f6c885416ae17e` exposes the
following distinct attribute domains:

| Interface | Representation | Meaning at this revision |
| --- | --- | --- |
| `hsa_amd_agent_set_attribute` with `HSA_AMD_AGENT_ATTRIBUTE_REQUEST_PERSISTING_L2_CACHE_SIZE` | Setter attribute `0`; input points to `size_t` bytes. | Request a size for a GPU agent. |
| `hsa_agent_get_info` with `HSA_AMD_AGENT_INFO_REQUEST_PERSISTING_L2_CACHE_SIZE` | Query `0xA125`; output is `size_t`. | Last successfully set request; initially zero. |
| `hsa_agent_get_info` with `HSA_AMD_AGENT_INFO_MAX_PERSISTING_L2_CACHE_SIZE` | Query `0xA126`; output is `size_t` bytes. | Maximum obtained from the agent's cache properties. |

[Setter declaration][hsa-setter] · [Query declarations][hsa-queries]
[Request initialization][agent-initialization] · [Query implementation][agent-queries]

The public setter requires an initialized runtime, a nonnull value and a live
GPU agent. `GpuAgent::SetAgentAttribute` rejects a request above the discovered
maximum, calls the driver's `SetPersistingCacheSize`, and updates its stored
request only after success. Driver failure becomes
`HSA_STATUS_ERROR_INVALID_ARGUMENT`; the preceding requested value remains
unchanged. [Public entry point][hsa-entry] · [Agent setter][agent-setter]

The requested-size query reads that stored value. It is neither hardware
readback nor a measurement of occupied cache lines. The setter receives no
queue, stream, address window, allocation, or completion signal, and its body
performs no queue drain. An application cannot derive an address-window policy
or an execution dependency from a successful size request alone.
[Agent queries and setter][agent-request-owner]

### Maximum discovery and partition capacity

ROCr filters the native cache list to GPU data caches and returns the first
nonzero `PersistingCacheSizeMax` from a level-2 record. It performs no sum or
unit conversion on this field. The thunk populates the 32-bit field from
`persisting_cache_size_max` in a node's cache properties. The topology snapshot
zero-initializes cache records, so a missing property remains zero.
[Cache-list owner][agent-cache-discovery] · [Native field][thunk-cache-field]
[Sysfs parser][thunk-cache-parser] · [Snapshot allocation][thunk-cache-allocation]

Ordinary cache capacity follows another calculation.
`HSA_AGENT_INFO_CACHE_SIZE` sums level-2 and higher cache records for the
partition and converts their `CacheSize` values from KiB to bytes. Thus a
partition's aggregate L2 capacity and the first nonzero persisting maximum
have different discovery rules. Neither query is a sum of already admitted
reservations. [Ordinary cache-size query][agent-cache-capacity]

### Render-node request and native admission

`KfdDriver::SetPersistingCacheSize` delegates to
`hsaKmtSetPersistingCacheSize`. Despite the driver class name, the thunk sends
this operation through an AMDGPU **render-node** file descriptor, using
`DRM_IOCTL_AMDGPU_VM`, rather than through the KFD device descriptor.
[Driver delegation][kfd-setter] · [Thunk operation][thunk-setter]

The thunk obtains the node's existing render descriptor from its memory
context. Primary-context device initialization can share the libdrm device
owner; this request does not create a private descriptor or cache allocation.
It rejects sizes above `UINT32_MAX`, then writes the requested bytes to
`args.in.size` and selects the symbolic operation
`AMDGPU_VM_OP_GL2_PERSISTING_L2_CACHE`. It supplies no rounding or completion
wait. [Render-descriptor owner][render-owner] · [Descriptor lookup][render-lookup]
[Request construction][thunk-setter]

The native interface must independently define that operation and its input
layout. The following public driver revisions have only
`AMDGPU_VM_OP_RESERVE_VMID` and `AMDGPU_VM_OP_UNRESERVE_VMID` in this ioctl;
their input contains `op` and `flags`, with no `size` member:

| Native source | Revision | VM interface and owner |
| --- | --- | --- |
| Upstream Linux | `fe2ec83746e501645709761605c2464a44fd2929` | [UAPI][linux-vm-uapi]; [handler][linux-vm-handler]. |
| ROCm AMDGPU | `820212794d184710298efcecce42c0215bfc9c6a` | [UAPI][rocm-vm-uapi]; [handler][rocm-vm-handler]. |
| ROCm AMDGPU, `releases/therock-7.14` | `5081d704e60e807e9541c355c53df57d88ea8cde` | [UAPI][therock-vm-uapi]; [handler][therock-vm-handler]. |

Their cache-property producers also omit `persisting_cache_size_max`.
[Linux producer][linux-cache-topology] · [ROCm producer][rocm-cache-topology]
[TheRock-release producer][therock-cache-topology]
Combined with the thunk's zero initialization and ROCr's maximum check, this
leaves no positive persisting-size request admitted by that discovery path.
Passing zero is not a special release path in the thunk; it still attempts
the same native operation.

These revisions therefore do not supply a native reservation recipe for the
ROCr interface. Its symbolic operation name does not establish a numeric
opcode, expanded ioctl layout, hardware size granularity, supported GC or
firmware revision, or a cache-capacity formula. A matching driver is also
needed to determine whether the hardware setting belongs to a VM or the
device, how competing requests interact, what reset preserves, and how
zero-size requests or owner destruction release it. The render descriptor
identifies the request's native context; it cannot by itself prove those
hardware lifetime rules.

ROCr's Virtio and XDNA driver overrides return
`HSA_STATUS_ERROR_INVALID_AGENT` for this driver operation. Their presence in
the common driver interface does not provide another implementation of the
cache control. [Virtio override][virtio-setter] · [XDNA override][xdna-setter]

## HIP capacity and access-window queries

At the same ROCm systems revision, HIP initializes
`hipDeviceProp_t::persistingL2CacheMaxSize` from CLR's `info.l2CacheSize_`.
That field comes from element 1 of the `HSA_AGENT_INFO_CACHE_SIZE` result,
the ordinary partition L2 capacity described above, rather than from the HSA
persisting-maximum query. HIP also sets `accessPolicyMaxWindowSize` to zero.
[CLR capacity source][clr-cache-capacity] · [HIP properties][hip-cache-properties]

The consuming API paths explain the consequence:

| Caller | Admission at the cited revision |
| --- | --- |
| `hipDeviceSetLimit` | Implements stack size, heap size and current scratch threshold; no persisting-cache quota case. [Limit setter][hip-limit-setter] |
| `hipStreamSetAttribute` | Accepts synchronization policy; an access-policy window reaches the invalid-value result. [Stream setter][hip-stream-setter] |
| `hipGraphKernelNodeSetAttribute` | Delegates access-window validation to `GraphKernelNode::SetAttrParams`, which bounds `num_bytes` by `accessPolicyMaxWindowSize`. A nonempty window is rejected; storing an empty window describes no address range. [Graph entry][hip-graph-entry] · [Window validation][hip-graph-window] |
| Extended kernel launch attribute processing | The ordinary runtime and module launch switches implement cooperative launch, clusters and dynamic prefetch. An access-policy window processed by either switch reaches the unsupported-attribute result. [Runtime launch][hip-runtime-launch] · [Module launch][hip-module-launch] |

The property, attribute spelling and an implemented window are consequently
separate facts. A nonzero `persistingL2CacheMaxSize` alone cannot establish
native reservation support or a usable HIP access window.

## Reuse-oriented execution sequence

For a repeatedly consumed GPU working set, the source-backed sequence keeps
memory ownership independent of replacement priority:

1. The native memory owner supplies accessible backing and mappings for the
   data, program and control words. Placement and coherence follow the
   [native memory policy](recipes/local-memory.md).
2. The producer publishes initialized data through the required
   [release and dependency](recipes/host-device.md). The consumer acquires
   it before the first read.
3. The shader selects the target's temporal hints for accesses whose reuse
   pattern is known. This choice changes replacement policy without granting
   additional capacity or transferring storage ownership.
4. A subsequent producer waits for the final reader's completion before
   modifying or reusing backing. The next consumer performs its own required
   acquisition, whether or not the previous contents remained cached.

The [resident shader protocol](shader-memory.md#resident-transfer-and-reuse)
and [multi-device pipeline ownership](../interop/pipelines.md) define those
publication and final-use edges. A native persisting-size control can be added
to such a flow only with its own demonstrated admission and lifetime contract;
the ISA hint does not fill in the missing driver semantics.

[rdna4-policy]: https://gpuopen.com/download/rdna4-instruction-set-architecture.pdf#page=51
[cdna5-policy]: https://www.amd.com/content/dam/amd/en/documents/instinct-tech-docs/instruction-set-architectures/amd-instinct-cdna5-instruction-set-architecture.pdf#page=45
[hsa-setter]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L4403-L4417
[hsa-queries]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L995-L1005
[agent-initialization]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L104-L135
[agent-queries]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2745-L2752
[hsa-entry]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L1848-L1865
[agent-setter]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2760-L2782
[agent-request-owner]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2745-L2782
[agent-cache-discovery]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L681-L717
[thunk-cache-field]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/include/hsakmt/hsakmttypes.h#L446-L459
[thunk-cache-parser]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/topology.c#L1621-L1685
[thunk-cache-allocation]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/topology.c#L2111-L2126
[agent-cache-capacity]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2423-L2439
[kfd-setter]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/driver/kfd/amd_kfd_driver.cpp#L1104-L1108
[thunk-setter]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/memory.c#L289-L325
[render-owner]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/fmm.c#L3225-L3293
[render-lookup]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/fmm.c#L4160-L4169
[linux-vm-uapi]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/include/uapi/drm/amdgpu_drm.h#L589-L607
[linux-vm-handler]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L2902-L2925
[rocm-vm-uapi]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/include/uapi/drm/amdgpu_drm.h#L637-L655
[rocm-vm-handler]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L2910-L2933
[therock-vm-uapi]: https://github.com/ROCm/amdgpu/blob/5081d704e60e807e9541c355c53df57d88ea8cde/include/uapi/drm/amdgpu_drm.h#L632-L650
[therock-vm-handler]: https://github.com/ROCm/amdgpu/blob/5081d704e60e807e9541c355c53df57d88ea8cde/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L2909-L2932
[linux-cache-topology]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L324-L358
[rocm-cache-topology]: https://github.com/ROCm/amdgpu/blob/820212794d184710298efcecce42c0215bfc9c6a/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L340-L374
[therock-cache-topology]: https://github.com/ROCm/amdgpu/blob/5081d704e60e807e9541c355c53df57d88ea8cde/drivers/gpu/drm/amd/amdkfd/kfd_topology.c#L340-L374
[virtio-setter]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_virtio_driver.h#L126-L128
[xdna-setter]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_xdna_driver.h#L182-L184
[clr-cache-capacity]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L1667-L1684
[hip-cache-properties]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_device.cpp#L767-L800
[hip-limit-setter]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_device_runtime.cpp#L654-L682
[hip-stream-setter]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_stream.cpp#L948-L985
[hip-graph-entry]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_graph.cpp#L1926-L1943
[hip-graph-window]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_graph_internal.hpp#L2064-L2097
[hip-runtime-launch]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_module.cpp#L1396-L1433
[hip-module-launch]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_module.cpp#L1495-L1531
