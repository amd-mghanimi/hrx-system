# AQL metadata-prefetch rings

An AMD metadata-prefetch ring gives the command processor a second record for
each AQL packet: a completion-event ID, a copied kernel-descriptor suffix,
selected argument DWORDs, and optional launch controls. The primary AQL packet
still supplies dispatch geometry, resource totals, executable and argument
addresses, ordering, and completion. Publishing metadata prepares those inputs;
it neither launches an independent task nor completes the dispatch.
[Metadata ABI][metadata-abi] · [CLR dispatch producer][clr-dispatch]

## Applicability and native enablement

ROCr's `AqlQueue` constructor selects metadata version 0.0 for dispatch and
barrier records when three conditions hold: its native-interface capability
predicate is true, the creation path requests metadata, and the reported agent
ISA has major version 12 and minor version at least 5. Other paths retain the
unsupported version value `0xFF` and no metadata-ring allocation. This is a
runtime predicate on the agent's ISA identity, not a rule that every CDNA or
RDNA queue has metadata. [Constructor selection][rocr-select] · [Version
representation][rocr-version]

The pinned `hsa_amd_queue_create` compute path passes `true` for that request;
its caller does not set a separate metadata flag. CLR obtains the returned
ring address when its `DEBUG_CLR_ENABLE_KDQ` setting is enabled and uses a
nonnull address as the producer gate. Creation, returned resource availability,
and the client's decision to populate it are separate steps.
[API creation caller][rocr-create] · [Agent forwarding][rocr-forward] ·
[CLR queue queries][clr-queries] · [Producer gate][clr-preloader]

`HSA_AMD_QUEUE_INFO_PREFETCH_METADATA_RING_BUFFER` returns the base as a
`uint64_t`. The corresponding `DISPATCH_PKT_VERSION_MAJOR/MINOR` and
`BARRIER_PKT_VERSION_MAJOR/MINOR` queries, under the same
`HSA_AMD_QUEUE_INFO_PREFETCH_METADATA_` prefix, return separate `uint8_t`
versions; `0xFF` means unsupported. CLR caches the dispatch version for its
producer. The public API keeps the two record families' versions distinct.
[Queue-info contract][metadata-query] · [CLR query caller][clr-queries]

| Layer | Predicate or native representation |
| --- | --- |
| ROCr native-interface predicate | KFD major greater than 1, or major 1 and minor at least 19. |
| Thunk creation | Nonzero metadata size requires its KFD minor-19 check and page-aligned size. |
| Upstream Linux UAPI history | Records metadata queue creation under version 1.22; the pinned interface is 1.23. |
| KFD queue properties | Copies `metadata_ring_size` only for `KFD_IOC_QUEUE_TYPE_COMPUTE_AQL`. The ioctl field is a 32-bit byte count. |
| GC12.1 MQD builder | Enables the metadata fetcher only when metadata bytes equal four times primary-ring bytes. |

[ROCr version predicate][rocr-kfd-version] · [Thunk checks][thunk-checks] ·
[Linux UAPI][linux-uapi] · [Queue properties][linux-properties] ·
[MQD construction][linux-mqd]

The 1.19 runtime threshold and 1.22 upstream UAPI history differ. These sources
do not establish which backported native interfaces satisfy the earlier
threshold. A runtime version check alone therefore does not identify the
upstream introduction or prove that the selected native queue has an enabled
metadata fetcher. [ROCr predicate][rocr-kfd-version] · [Linux history][linux-uapi]

Linux selects its V12.1 queue-manager operations for native GC IP at least
12.1.0. That physical-IP predicate and ROCr's ISA 12.x predicate use different
version spaces. On initial queue creation, the cited V12.1 builder logs a
warning for a nonzero size with the wrong 4:1 ratio instead of enabling the
fetcher; it does not return a queue-creation error. [Native selection][linux-selection]
· [Size and enablement][linux-mqd]

## Ring layout and backing

For `N` power-of-two primary slots at GPU virtual address `A`, ROCr allocates
both rings together:

| Quantity | Byte layout |
| --- | --- |
| Primary AQL ring | `64 * N` bytes at `A`. |
| Metadata ring | `256 * N` bytes at `M = A + 64 * N`. |
| Complete backing | `320 * N` bytes; five times the primary-ring storage. |
| Packet ID `P` | Physical index `I = P & (N - 1)`. |
| Corresponding records | Primary packet at `A + 64 * I`; metadata at `M + 256 * I`. |

The allocator asserts that both ring sizes are multiples of 4096 bytes. Its
native MQD uses `(A + primary_bytes) >> 8` for `CP_HQD_KD_BASE` and
`CP_HQD_KD_BASE_HI`, sets `CP_HQD_KD_CNTL.KD_FETCHER_ENABLE`, and encodes
`KD_SIZE = 2` for a 64-DWORD metadata record. There is no independently supplied
metadata base or separately reserved metadata index in this flow.
[Allocation][rocr-allocation] · [Paired producer indexing][clr-dispatch] ·
[Native base and size][linux-mqd]

System-backed rings use ROCr's nonpaged, executable allocation. Device-backed
rings require Large BAR and use executable, GPU-uncached allocation; GPU UC
does not specify the CPU mapping's cache type. KFD acquires backing for the
page-rounded sum of both ring extents. These are mapped queue resources, not
ordinary pageable host pointers. The [queue-storage contract](../architectures.md#kfd-queue-storage)
and [host-store ordering](publication.md#ring-placement-and-x86-stores) provide
the surrounding mapping rules. [Allocator flags][rocr-allocation] · [Native
backing acquisition][linux-backing]

ROCr initializes the type byte in all four metadata headers to
`HSA_PACKET_TYPE_INVALID = 1`. It does not zero the entire allocation.
Producers still own every required-zero field of the record they publish.
[Initial headers][rocr-initialize]

## Version-0.0 record representation

Metadata records have four 32-bit headers at byte offsets 0, 64, 128, and 192.
The public ABI requires `header1`, `header2`, and `header3` to equal `header0`.
Their bits differ from an AQL packet header: metadata carries type and version,
not the AQL barrier or acquire/release scopes.

| Bits in each metadata header DWORD | Public field |
| --- | --- |
| 7:0 | `HSA_AMD_METADATA_PACKET_HEADER_TYPE`, an HSA packet type. |
| 23:8 | Reserved. |
| 26:24 | `HSA_AMD_METADATA_PACKET_HEADER_VERSION_MAJOR`, 3 bits. |
| 31:27 | `HSA_AMD_METADATA_PACKET_HEADER_VERSION_MINOR`, 5 bits. |

[Header offsets and widths][metadata-header]

CLR builds the word with these public shifts and the primary packet's type.
ROCr's separate internal `AqlMetadataPrefetchPacket::header_t` declares its
last byte as minor:5 followed by major:3, the opposite field order from the
public shifts on its little-endian target. The selected version is 0.0, so
this disagreement does not alter that version's word; it prevents deriving
nonzero version encodings from the internal overlay. [CLR version word][clr-queries]
· [Internal overlay][rocr-overlay] · [Selected version][rocr-select]

The dispatch record has the following layout. A DWORD is 32 bits.

| Byte offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | `header0`. |
| 4 | 4 | `event_id`, copied from `amd_signal_t.event_id` for the completion signal, or zero. |
| 8 | 48 | `kernel_descriptor`, the suffix corresponding to bytes 16–63 of the compiler descriptor. |
| 56 | 8 | Reserved, zero. |
| 64 | 4 | `header1`. |
| 68 | 60 | `kernarg_preload_0_14`, 15 argument DWORDs. |
| 128 | 4 | `header2`. |
| 132 | 60 | `kernarg_preload_15_29`, 15 argument DWORDs. |
| 192 | 4 | `header3`. |
| 196 | 8 | `kernarg_preload_30_31`, 2 argument DWORDs. |
| 204 | 52 | Zero reserved storage or `amd_launch_descriptor_t`. |

[Dispatch representation][metadata-abi] · [Event and payload builder][clr-payload]

Within the 48-byte descriptor suffix, the signed code-entry offset is at byte
0, the 20 reserved bytes at 8, `COMPUTE_PGM_RSRC3/1/2` at 28/32/36, kernel
properties at 40, preload specification at 42, and four reserved bytes at 44.
CLR copies these bytes unchanged from the host-accessible compiler descriptor
and keeps the original `kernel_object` in AQL. The copied relative entry offset
does not turn metadata storage into a standalone executable descriptor.
[Suffix declaration][metadata-descriptor] · [Load-time descriptor lookup][clr-kernel]
· [Copy operation][clr-payload] · [Executable ABI](dispatch.md#packet-and-executable-representation)

The preload specification has a 7-bit DWORD count and a 9-bit DWORD offset.
CLR copies from `kernarg_address + 4 * offset`, caps the copied count at 32,
and, when capping, changes the count in the metadata copy. The seven-bit
representable count is therefore distinct from this record's 32-DWORD
capacity. A shader can still read argument storage beyond the copied subset;
preloading does not shorten the [argument lifetime](dispatch.md#arguments-geometry-and-initial-registers).
[Preload representation][metadata-descriptor] · [Copy and cap][clr-preload-copy]

The barrier record is also 256 bytes. It contains `header0` and `event_id` at
0 and 4, followed by required-zero bytes 8–63, 68–127, 132–191, and 196–255.
The intervening headers remain at 64, 128, and 192. Dependency signal handles,
comparisons, fence scopes, and completion handles remain in the primary AQL
packet. [Barrier representation][metadata-barrier] · [Dependency packets](barriers.md)

## Host publication and slot reuse

CLR reserves an ordinary AQL packet ID, waits for primary-ring capacity, and
uses that masked index for both records. `WaitForQueueSlot` reads the ordinary
AQL read index with acquire ordering when its cached frontier is insufficient.
The direct caller uses `N - 1` as capacity. It neither polls a separate metadata
frontier nor waits for dispatch completion before every publication.
[Reservation and caller][clr-dispatch] · [Capacity observation][clr-capacity]

For a kernel dispatch, the pinned writer selects these store sequences:

| Destination and host path | Metadata and primary-packet publication |
| --- | --- |
| System memory, split AQL stores | Copy the AQL body; fill metadata body; `SFENCE`; write metadata headers 3, 2, 1, 0; `SFENCE`; release-store the valid AQL first DWORD. |
| Device memory, non-temporal stores | Copy the AQL body; build a zeroed 256-byte metadata record locally; copy its four 64-byte segments, each carrying a valid header; `SFENCE`; release-store the valid AQL first DWORD. |
| Device memory, MOVDIR64B | Build the same local metadata record; issue four 64-byte direct stores; `SFENCE`; publish the complete AQL packet with one 64-byte direct store. |

[Metadata writer][clr-metadata-write] · [Outer publication sequence][clr-publish]

The device-memory non-temporal path does not put a fence between each metadata
segment's body and header. CLR explicitly relies on its staged 64-byte store
path for that publication; it is different from the direct system-memory
writer. The four MOVDIR64B operations also remain four operations, not an
atomic 256-byte publication. These are the cited x86 producer strategies, not
a portable guarantee for arbitrary CPU stores. Normal notification follows
through the [doorbell protocol](publication.md#doorbell-values-and-native-mappings).
[Staging rationale and stores][clr-metadata-write] · [Notifier][clr-publish]

### Barrier writer and the four-header declaration

CLR's direct barrier overload writes only `event_id` and `header0`, or one
zero-initialized 64-byte segment with MOVDIR64B. It does not update headers
1–3 or clear all of the reserved regions described by the public barrier
structure. This applies to the overload used for AND, OR, and AMD barrier-value
packets. The builder and declaration therefore describe different byte writes.
[Overload selection and writer][clr-barrier] · [Public barrier layout][metadata-barrier]

Neither cited source supplies a firmware rule saying that the other three
headers and reserved regions are ignored for these barrier types. That
missing rule is material to dispatch-to-barrier slot reuse: the direct writer
alone cannot establish that stale dispatch contents are harmless. The public
four-header/zero-field representation and this consumer's abbreviated writer
remain separate evidence, rather than an inferred permission to omit fields.
[Initial contents][rocr-initialize] · [Direct writer][clr-barrier]

### Reuse and final ownership

The CLR caller uses the AQL read frontier to reuse the paired metadata slot.
Its dispatch writer also assumes that the metadata headers remain INVALID
while rebuilding the body. The public metadata declarations do not state the
fetcher's header-invalidation point or its relation to AQL read-index
advancement. Those source-visible assumptions identify the firmware contract
needed for an independent producer; ordinary AQL's packet-retirement rule
does not itself specify a second ring's fetch protocol.
[Reuse caller][clr-dispatch] · [Header assumption][clr-payload] · [Metadata ABI][metadata-abi]

Even when paired-slot reuse is established, executable code, arguments,
completion signals, and payloads retain their own final users. The metadata
copy contains selected values, not an ownership transfer for every address in
the dispatch. Task completion and visibility still follow the primary packet's
dependency and fence semantics. [Primary packet ownership](publication.md#packet-publication)
· [Dispatch lifetime](dispatch.md#executable-publication-and-final-use)

The successful queue teardown path removes the native queue before freeing
the combined allocation. ROCr's `Inactivate` calls the native destroy operation;
`FreeQueueMemory` frees the primary base and clears both ring pointers. Its
native destroy result is asserted rather than propagated, so the wrapper's
returned success alone does not prove native removal when that assertion is
disabled. Queue destruction also does not substitute for obtaining successful
results from every submitted task. [Destructor][rocr-destroy] · [Native destroy
call][rocr-inactivate] · [Shared free][rocr-free] · [Native ownership](../context-save.md#save-resume-and-final-ownership)

## Optional launch descriptor

The final 52 bytes of dispatch metadata overlay `amd_launch_descriptor_t`.
Its version is independent of the metadata header's version: metadata 0.0
can carry launch version 1. The public header assigns launch version 0 to no
descriptor and version 1 to the gfx1250 layout. CLR selects version 1 with its
ISA-major-12/minor-at-least-5 predicate, but populates it only when dynamic
data prefetch is requested. Ordinary construction leaves this area zero.
[Versions][launch-versions] · [Caller selection][clr-launch-version] · [Builder][clr-payload]

| Launch-relative byte offset | Size | Declared field |
| --- | ---: | --- |
| 0, 1, 2 | 1 each | Version, priority, power-management hint. Zero priority/hint means default. |
| 3 | 1 | Reserved, zero. |
| 4 | 4 | CU enable: start bits 3:0, count 7:4, shader-engine enables 9:8, reserved 31:10. Zero bypasses this override. |
| 8 | 2 | Dispatch granularity limiter; zero leaves default policy. |
| 10–13 | 4 | Reserved, zero. |
| 14 | 2 | `tg_chunk_size`; the field comment describes workgroups per XCC and zero inheritance. |
| 16 | 4 | Reserved, zero. |
| 20, 36 | 16 each | Two `amd_data_prefetch_t` entries. |

[Launch declaration][launch-layout]

The declaration has unresolved semantic conflicts. Its version-1 summary calls
bytes 12–19 reserved, but the same structure assigns `tg_chunk_size` to 14–15.
The limiter comment counts clusters as units, then says the value cannot be
less than one cluster's size; those are not the same quantity. Nonzero priority
encoding is left unspecified. The inspected CLR builder leaves these launch
controls zero, so it does not resolve their nondefault behavior.
[Version summary][launch-versions] · [Limiter and chunk comments][launch-controls]
· [Producer values][clr-payload]

Each prefetch entry occupies four DWORDs:

| DWORD / bits | Field and declared unit |
| --- | --- |
| 0 / 0 | Cooperative hint: 0 requests the full region per XCC; 1 distributes work across XCCs. The header says CPX ignores it. |
| 0 / 2:1 | Temporal policy: 0 regular, 1 non-temporal, 2 high-temporal, 3 last-use. |
| 0 / 4:3 | Scope: 0 and 1 reserved, 2 DEVICE, 3 SYSTEM. |
| 0 / 15:5 | Burst bytes in 256-byte units, minus one. |
| 0 / 31:16 | Burst count, minus one. |
| 1 / 3:0 | Log2 bursts per chunk, 0–15. |
| 1 / 5:4 | Translation permissions: 0 read, 1 write, 2 execute; the header assigns no meaning to 3. |
| 1 / 7:6 | Reserved, zero. |
| 1 / 31:8 | Address low field: VA bits 31:8 in absolute mode. |
| 2 / 24:0 | Address high field: VA bits 56:32 in absolute mode. Bits 31:25 are reserved. |
| 3 / 23:0 | Stride in 256-byte units. Bits 29:24 are reserved. |
| 3 / 31:30 | Mode: 0 disabled, 1 absolute VA, 2 kernarg-pointer offset, 3 reserved. |

In mode 2 the low address field instead carries an 8-byte-aligned byte offset
into the argument buffer; its upper 12 bits and the high address field are
zero. The header describes fetching an 8-byte pointer there. This is distinct
from the descriptor's DWORD-based argument-preload offset.
[Prefetch declaration][prefetch-layout]

CLR's actual builder uses absolute VA, cooperative=1, scope=2 (DEVICE), the
requested temporal policy's low two bits, and zero-initialized chunk size and
translation permissions. Thus it emits permission value 0 (read), while the
header labels value 2 (execute) as expected. The builder does not exercise
kernarg-offset addressing. The public geometry and mode declarations do not,
by themselves, establish performance, residency guarantees, or completion
ordering for the prefetched payload. [Concrete producer][clr-prefetch] ·
[Declared modes and permissions][prefetch-layout]

Return to [AQL](README.md), [packet publication](publication.md), or
[kernel dispatch](dispatch.md).

[metadata-header]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L440-L476
[metadata-descriptor]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L478-L500
[metadata-abi]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L502-L559
[metadata-barrier]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L561-L604
[metadata-query]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L4917-L4945
[rocr-select]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L119-L129
[rocr-version]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_aql_queue.h#L381-L403
[rocr-kfd-version]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/runtime.h#L554-L567
[rocr-create]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L2575-L2588
[rocr-forward]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2849-L2850
[rocr-overlay]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/queue.h#L211-L234
[rocr-allocation]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L596-L634
[rocr-initialize]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L636-L651
[rocr-destroy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L380-L400
[rocr-inactivate]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L740-L748
[rocr-free]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L655-L677
[thunk-checks]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/libhsakmt/src/queues.c#L700-L728
[linux-uapi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L43-L93
[linux-properties]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L234-L284
[linux-selection]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L3312-L3322
[linux-mqd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v12_1.c#L247-L275
[linux-backing]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_queue.c#L245-L273
[clr-queries]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocdevice.cpp#L3587-L3611
[clr-preloader]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.hpp#L336-L414
[clr-kernel]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rockernel.cpp#L51-L62
[clr-dispatch]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1524-L1574
[clr-capacity]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.hpp#L862-L877
[clr-publish]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1436-L1470
[clr-payload]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L5643-L5684
[clr-preload-copy]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L5686-L5738
[clr-metadata-write]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L5744-L5810
[clr-barrier]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.hpp#L442-L508
[clr-launch-version]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2531-L2533
[clr-prefetch]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L5619-L5639
[launch-versions]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_launch_descriptor.h#L30-L46
[launch-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_launch_descriptor.h#L219-L304
[launch-controls]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_launch_descriptor.h#L314-L389
[prefetch-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/inc/amd_launch_descriptor.h#L52-L174
