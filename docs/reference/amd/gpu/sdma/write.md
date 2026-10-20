# SDMA inline data writes

`WRITE_LINEAR`, named `SDMA_PKT_WRITE_UNTILED` in the packet definitions,
copies DWORD values carried inside the command stream to a writable GPU
address. It has no separate source allocation. Command storage supplies both
the operation and its input data; destination visibility, downstream readers
and final storage reuse have separate owners. Legacy SI uses a different
header. [KFD builder][builder] [SI emitter][linux-si]

An API named “update memory” need not select this operation. PAL and RADV's
ordinary SDMA updates copy host values into GPU upload storage and submit
COPY packets. Actual inline writers include driver metadata updates, private
queue controls and KFD's USER-queue utility. Their source-storage and
completion contracts differ. [PAL update][pal-update]
[RADV update][mesa-update]

## Applicability and count families

For `N` inline DWORDs, the native producer selects both the header shape and
the encoded count. The following are source-selected representations, not
one count rule derived from a compiler target's name.

| Native source family | Fixed words before data | Count representation |
| --- | --- | --- |
| SI `DMA_PACKET_WRITE` | Three: legacy header, destination low, destination high. | Direct `N` in header bits 19:0; opcode 2 in bits 31:28. The ordinary writer clears the other header controls. |
| CIK native emitter | Four, using opcode 2/suboperation 0. | Direct `N` in DWORD 3; the emitter writes the word without a generated count mask. |
| SDMA2.4/3.0 with Iceland/Tonga definitions | Four. | Direct `N` in DWORD 3 bits 21:0, a 22-bit field. [2.4 emitter][linux24] [3.0 emitter][linux30] |
| SDMA4.0/4.4.2/5.0/5.2/6.0/7.0/7.1 native emitters | Four. | `N - 1` in DWORD 3; the corresponding Vega/Navi/SDMA6/7.1 definitions name bits 19:0, a 20-bit field. [4.0][linux40] [4.4.2][linux442-write] [5.0][linux50] [5.2][linux52] [6.0][linux6-write] [7.0][linux70] [7.1][linux71] |

[SI header][si-encoding] [CIK emission][linux-cik]
[Iceland fields][linux-iceland] [Tonga fields][linux-tonga]
[Vega fields][linux-vega] [Navi fields][linux-navi]
[SDMA6 fields][linux6-fields] [SDMA7.1 fields][linux71-fields]

SI's generic header helper also exposes `b`, `t` and `s` at bits 26, 23 and
22; the ordinary writer passes zero for each. Its native writeback check
masks the address-high word to eight bits, while its page-table writer emits
the full upper word. The check alone therefore does not establish a universal
40-bit WRITE address format. CIK's cited emitter establishes direct count,
but not the complete legal count/policy width. [SI check][linux-si-check]
[SI writer][linux-si] [CIK encoding][cik-encoding]

The modern 20-bit minus-one field represents `1..2^20` DWORDs: at most
4 MiB of inline data, plus its command header. The older 22-bit direct field
represents positive counts through `2^22 - 1` DWORDs. Those are encoded
capacities; a ring reservation, command-buffer limit or actual caller can
impose a much smaller bound. A nonempty packet requires all declared inline
words. Zero is one DWORD in the minus-one form, so it cannot encode empty work.

KFD's utility retains a **20-bit** count view even on its older direct-count
branch. It selects direct `N` before `FAMILY_AI` and `N - 1` from that family;
it has no chunk loop or empty-input guard. Its ordinary scalar caller passes
exactly one DWORD. These helper limits do not redefine Iceland/Tonga's
22-bit hardware declarations. [Builder][builder] [Utility fields][fields]
[Scalar caller][caller]

## Representation

The four-word form consists of the following fixed header followed by exactly
`N` data DWORDs, or `4 * N` payload bytes. The selected native callers use
DWORD-aligned destinations; their examples do not establish the legality of
unaligned addresses. The native five-DWORD C structures include the first data
word as a placeholder; their `sizeof` is not the length of an arbitrary inline
packet. [KFD builder][builder]
[PAL builder][pal-builder] [Native destination][linux-si-check]

| DWORD | Fields and units |
| --- | --- |
| 0 | Opcode 2 in bits 7:0; suboperation 0 in bits 15:8. Generation-specific controls appear below. |
| 1–2 | Low and high halves of the absolute GPU destination byte address. |
| 3 | Direct or minus-one DWORD count at the width selected above; upper policy fields depend on the native layout. |
| 4 through `N + 3` | The `N` DWORD values, in destination order. |

The producer reserves all `4 + N` words before publication. For three values,
KFD's older direct form is `[2, A_lo, A_hi, 3, D0, D1, D2]`; its unscoped
minus-one form changes the fourth word to `2`. The scoped utility form uses
`0x0c000002` there instead. Each packet occupies seven DWORDs, and the next
command begins immediately after `D2`. No inline word names a source pointer
or an indirect command stream. [Builder][builder]
[Ring reservation][kfd-placement] [USER publication](publication.md)

## Native layout and caller differences

Named policy fields vary in both the header and count word. Unnamed bits are
reserved in the cited layout; the ordinary zero-initialized builders leave
unselected controls zero. A union overlay gives alternative meanings to the
same bits, not independently composable operands.

| Source-selected layout | Header controls beyond opcode/suboperation | Count-word controls beyond COUNT |
| --- | --- | --- |
| Linux Iceland/Tonga | No other named header control. | `sw` at bits 25:24. |
| Linux Vega/Navi; PAL GFX10/GFX11 | `encrypt` at bit 16; `tmz` at bit 18. PAL's GFX10.3+ overlay additionally names `cpv` at bit 28. | `sw` at bits 25:24; PAL's GFX10.3+ overlay names `cache_policy` at bits 28:26. |
| Linux SDMA6/7.1 | `encrypt` at bit 16; `tmz` at bit 18; `cpv` at bit 28. | `sw` at bits 25:24; `cache_policy` at bits 28:26. |
| PAL GFX12 | `nopte_comp` at bit 16; `tmz` at bit 18. | `sys` at bit 20, `snp` at bit 22, `gpa` at bit 23, `dst_mall_policy` at bits 29:28. |
| KFD utility | `tmz` at bit 18. | Older `sw` view at bits 25:24. Newer view: `sys` 20, `snp` 22, `gpa` 23, `mtype` 25:24, `scope` 27:26, `temp_hint` 30:28. |

[PAL GFX10 fields][pal-fields] [PAL GFX12 fields][pal12-fields]
[KFD fields][fields] [KFD system scope][scope] [KFD selection][builder]

The KFD builder selects SYS scope=3 when `m_FamilyId >= FAMILY_GFX125X`;
it does not separately set SYS or MTYPE. All other optional fields stay zero
in its `calloc`-allocated packet. Its node mapper assigns that family only
for EngineId **major 12, minor == 5**, and assigns `FAMILY_GFX12` for other
major-12 minors. This condition belongs to the utility's node-to-family
mapper; other clients' packet choices have their own selectors.
[Packet storage][allocation]
[Family mapping][kfd-family] [Queue family assignment][kfd-create]

Linux's cache-policy view does not corroborate KFD's scope interpretation at
bits 27:26, and PAL GFX12 assigns another policy to bits 29:28. Its header
also names bit 16 differently. The public definitions alone do not resolve
those applicability differences. Matching opcode and count fields does not
make the optional operands interchangeable. [Linux SDMA7.1 fields][linux71-fields]

## Publishers and input ownership

### Linux scheduled writes and KFD USER queues

Linux's ring checks write one inline DWORD to a driver-owned writeback slot.
Its scheduled IB checks additionally wait for the submission's native fence
before inspecting that slot. The production VM update owner chooses inline
`write_pte` only for fewer than three entries: positive updates carry two or
four DWORDs, because each entry is 64 bits. Larger contiguous updates use a
different operation. These driver-owned destinations and small witnesses
establish actual encoding choices, not an application mapping interface or
the maximum accepted inline size. [Ring check][linux442]
[Scheduled check][linux6-ib] [Production selection][linux-vm-selection]

The native job owns its IB storage through scheduler completion. The VM
owner separately publishes the finished fence for its update dependencies;
its table storage still has subsequent users. Native submission framing and
IB retirement are described with the [completion observer](fence.md#linux-ring-completion-and-user-fences).
[VM publication][linux-vm-publication]

KFD's `SDMAWriteDataPacket` utility has two host capture points. Construction
copies the caller's values into an allocated packet. `BaseQueue::PlacePacket`
then copies that packet into the mapped ring, reserving its entire extent
and any required NOP tail padding while leaving one ring slot unused. The
source array can expire after the first copy; the temporary packet can expire
after the second. Neither lifetime is the published ring's lifetime.
[Packet builder][builder] [Packet allocation][allocation]
[Ring placement][kfd-placement]

The utility publishes byte positions with barriers before WPTR and before
the doorbell, using its selected 32-bit or 64-bit resources. Its simple
scalar caller waits for ring consumption, then separately polls the
destination's 32-bit value before destroying the queue. That volatile value
poll contains no explicit acquire fence and supplies no general payload
acquire protocol. Queue destruction and destination allocation release have
distinct owners. The optional event path instead appends FENCE/TRAP; the
simple caller selects no event. [Publication/event path][publication]
[Scalar flow][caller] [Value observer][kfd-observer]
[Queue destruction][kfd-destroy] [Destination release][kfd-memory-release]

### RADV inline controls and metadata

Mesa's `ac_emit_sdma_write_data_head` writes a four-word header with `N - 1`
and no optional policy bits. Its RADV wrapper chooses SDMA only for an SDMA
command stream; the CP engine-selector and predication arguments do not
affect this arm. The wrapper reserves `4 + N` words, then either it or its
caller writes the data directly into mapped command storage. It neither
splits a large inline payload nor establishes a downstream dependency.
[Common header][mesa-write] [Stream selection and payload][mesa-wrapper]
[Mapped writes][mesa-embedding]

RADV's transfer-queue advertisement requires known SDMA, an available native
queue, application/profile opt-in, an enabled compute queue, GFX9 or newer,
and no transfer-disable flag. Its unconditional minus-one builder is used
inside that selected path; its existence does not extend the count convention
to older engines. [Queue selection][mesa-selection]

Actual inline uses include the following:

| Caller | Inline payload and owner |
| --- | --- |
| Command-buffer gang finalization | One zero DWORD on each participating stream. The transfer leader uses SDMA; its ACE follower uses CP. Progress publication uses separate FENCE/EOP operations. |
| Queue gang preamble/postamble | Single zero/one DWORDs coordinate the queue's persistent private words. The final leader wait joins the follower before resetting the word and permitting resubmission. |
| Applicable image metadata initialization | One, `L`, or `2L` DWORDs for selected clear values, HiZ/Z-range state, or 64-bit FCE/DCC predicates, where `L` is a mip-level count. The destination belongs to the image allocation. |

[Command-buffer reset writes][mesa-gang-reset]
[Queue preamble and final join][mesa-queue-gang]
[Depth metadata][mesa-depth] [FCE/DCC metadata][mesa-dcc]
[Color metadata][mesa-color]

Image transitions select the applicable metadata writers. Later CP register
loads, conditional execution and decompression predicates read that storage;
it remains live through those consumers across command buffers. A 64-bit
Boolean represented by two inline DWORDs is not thereby an atomic store.
Transfer barriers use RADV's pending-operation NOP policy and the surrounding
stage/access requirements; cross-queue ownership uses submission dependencies.
[Initialization][mesa-image-init] [Transition selection][mesa-transition]
[Metadata addresses][mesa-image-addresses] [Clear-value consumer][mesa-clear-consumer]
[Z-range conditional execution][mesa-zrange-consumer]
[HiZ conditional execution][mesa-hiz-consumer]
[Predicate consumer][mesa-predicate-consumer] [Transfer barrier][mesa-barrier]

The shared writer also appears in graphics/compute diagnostics and event
code. A syntactic SDMA arm alone does not establish that those operations
are legal transfer-queue callers. Native command storage is retained through
its submitted IBs; command-buffer reset/destruction does not supply a wait
for outstanding work. [Native command storage][mesa-command-storage]
[Submission extents][mesa-submission] [Reset ownership][mesa-command-reset]

### PAL metadata and separately embedded updates

PAL GFX10 contains a conditional inline write for image DCC state metadata.
Its caller requires tracking metadata, a non-decompressed color layout and
permission under `waSdmaPreventCompressedSurfUse`. At the cited revision,
the image-layout producer excludes `LayoutDmaEngine` from both compressed
color-state maps, and the copy API requires the actual engine bit in the
supplied layout. A valid ordinary DMA layout therefore does not select this
write. The [image-transfer admission rules](images.md#metadata-address-paths)
connect that producer and caller contract to the conditional builder.
[Inline builder][pal-builder] [Conditional caller][pal-metadata]

When selected, the builder copies a zero-initialized 16-byte record with
`isCompressed=1`: four DWORDs `[1, 0, 0, 0]`, count field 3, eight command
DWORDs total. The local record can expire once copied into the command
stream; the destination is part of the image's bound memory.
[Record layout][pal-metadata-record] [Bound destination][pal-metadata-address]

The conditional callsites follow destination image copies. Decompression
can consume this state as a graphics predicate; its compute path is
separate. That consumer does not establish reachability of the DMA write.
Both image storage and producer/consumer dependencies outlive the inline
record's host capture. PAL GFX12's ordinary update API uses the distinct
path below. [Conditional copy caller][pal-metadata-copy]
[Decompression consumer][pal-metadata-consumer]

Both GFX10 and GFX12 `CmdUpdateMemory` copy host values into GPU embedded
storage, then submit COPY packets from it. Their public contract names
DWORD-aligned byte offsets/sizes, Blt stage and CopyDst coherence. The
embedded chunk limit comes from command-allocator configuration, not the
WRITE count field. GFX12 additionally derives compression flags from the
source and destination allocations. [Public contract][pal-update-api]
[GFX10 update][pal-update] [GFX12 update][pal12-update]
[Embedded limit][pal-embedded-limit]

The cited update loops advance local source/destination cursors but continue
passing the original addresses to their COPY builder. If one embedded chunk
needs multiple packets, those calls revisit the original prefix. The COPY
caps are 4 MiB before PAL's GFX10.3+ predicate and 1 GiB afterward, including
GFX12; the allocator contract does not impose those caps on embedded chunks.
This is a source-level continuation discrepancy, not a WRITE hardware limit.
[GFX10 COPY cap][pal-copy-cap] [GFX12 COPY cap][pal12-copy-cap]
[Allocator contract][pal-allocator-api] [Allocator validation][pal-allocator-validation]

Concrete clients supply smaller bounds that prevent a second COPY within
one embedded piece:

| Client path | Bound and owner |
| --- | --- |
| XGL's `vkCmdUpdateBuffer` translation | Vulkan permits at most 65,536 bytes per call. XGL forwards that count to `CmdUpdateMemory`; the valid API bound holds even with larger configured embedded chunks. [Entry][xgl-update-entry] [Recording][xgl-update-recording] [PAL forwarding][xgl-update] [Vulkan contract][vulkan-update] |
| CLR's PAL command allocator | Both compute and DMA queues share an allocator with 64 KiB ordinary embedded chunks. This limits each piece, independently of the complete host-update size. [Allocator and queues][clr-embedded] |
| PAL's internal command-upload ring | Its DMA command buffers use the internal allocator, whose ordinary embedded chunks are 8 KiB. [DMA owner][pal-upload-owner] [Allocator construction][pal-internal-allocator] [Chunk size][pal-internal-chunk] |

These are client-specific bounds, not a numeric maximum in PAL's public
update API. The two SDMA update bodies also narrow the complete host byte
count to 32 bits before dividing by four; `2^32` bytes becomes zero remaining
DWORDs. XGL's per-call bound excludes that separate source-width discrepancy;
a small embedded chunk alone does not bound the complete update. Packet
capacity, embedded-piece size and host-API extent retain separate meanings.
[Public contract][pal-update-api] [GFX10 update][pal-update]
[GFX12 update][pal12-update]

Embedded GPU data remains an input allocation after the host copy returns.
PAL's public contract ends CPU recording access at `End` and GPU address
validity at `Reset`/`Begin`, with references restricted to that command
buffer. `End` attaches embedded chunks to the root command chunk. Automatic
reuse with busy tracking checks the root's completion; with
`disableBusyChunkTracking`, the client instead guarantees GPU completion
before returning chunks. Explicit retained-chunk reset likewise requires
the client to have ended every use. [Embedded allocation][pal-embedded-api]
[Root association][pal-embedded-root] [Return/reset][pal-embedded-return]
[Tracking policy][pal-tracking-policy] [Tracking selection][pal-tracking-selection]
[Completion owner][pal-root-owner] [Allocator reuse][pal-allocator-reuse]

RADV's successful transfer-buffer update similarly captures input into its
GPU upload allocation, then selects SDMA COPY. Older upload allocations
remain attached to the command buffer when it grows. The host input can
expire after capture, while upload storage remains live through COPY and
destination storage through its consumers. Its COPY chunk loop advances
both addresses; that chunk limit is independent of inline WRITE capacity.
[Update caller][mesa-update] [Upload owner][mesa-upload]
[Transfer selection][mesa-copy-select] [COPY advancement][mesa-copy-loop]

## Ordering, completion and ownership

A WRITE is transfer work. ROCr explicitly uses FENCE instead of WRITE for
completion because serial copy/write packets can overlap. Observing a later
WRITE is therefore not a general completion certificate for earlier work.
[Completion choice][completion] [Transfer ordering](ordering.md)

For the transfer-drain contract used by RADV, a stream updating data for a
subsequent SDMA copy has the following dependency shape:

```text
selected dependency and cache acquire
WRITE_LINEAR values -> A
NOP                            complete pending transfers before reading A
COPY_LINEAR A -> B
selected cache release
FENCE completion               publish completed payload to its observer
```

The selected NOP policy supplies the transfer dependency, while mapping
attributes and cache operations supply visibility. The final FENCE orders
the work and writes a separate completion word. These roles do not imply
identical optional-field semantics on another native transport.
[Transfer barrier][mesa-barrier] [Ordering boundary](ordering.md)
[Cache operations](cache.md) [Completion store](fence.md)

The original host array, command-carried input, separately uploaded input,
destination and completion word have distinct final users. A payload value
does not certify retirement of all those resources. The complete
[completion-store protocols](fence.md) identify the native observer,
notification tail and storage owner; [command publication](publication.md)
and [command buffers](command-buffers.md) supply the input-retirement contract.

[builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/SDMAPacket.cpp#L30-L70
[linux-si]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L346-L360
[pal-update]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1062-L1117
[mesa-update]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_buffer.c#L469-L515
[linux24]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v2_4.c#L679-L695
[linux30]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c#L953-L969
[linux40]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L1613-L1629
[linux442-write]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L1207-L1223
[linux50]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c#L1181-L1197
[linux52]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_2.c#L1080-L1096
[linux6-write]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1071-L1087
[linux70]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L1089-L1105
[linux71]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1077-L1093
[si-encoding]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sid.h#L557-L580
[linux-cik]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L744-L760
[linux-iceland]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/iceland_sdma_pkt_open.h#L1356-L1399
[linux-tonga]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h#L1356-L1399
[linux-vega]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L1548-L1603
[linux-navi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L2666-L2721
[linux6-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L3073-L3140
[linux71-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L3073-L3140
[linux-si-check]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L207-L247
[cik-encoding]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L495-L510
[fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/include/sdma_pkt_struct.h#L146-L215
[caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/KFDQMTest.cpp#L195-L213
[pal-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1033-L1057
[kfd-placement]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/BaseQueue.cpp#L160-L208
[pal-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L3452-L3520
[pal12-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L3476-L3535
[scope]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/include/sdma_pkt_struct.h#L37-L43
[allocation]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/BasePacket.cpp#L49-L60
[kfd-family]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/KFDTestUtil.cpp#L194-L249
[kfd-create]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/BaseQueue.cpp#L97-L103
[linux442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L1056-L1098
[linux6-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L964-L1030
[linux-vm-selection]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c#L187-L203
[linux-vm-publication]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm_sdma.c#L77-L145
[publication]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/SDMAQueue.cpp#L69-L98
[kfd-observer]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/KFDTestUtil.cpp#L85-L94
[kfd-destroy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/BaseQueue.cpp#L118-L131
[kfd-memory-release]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/tests/kfdtest/src/KFDTestUtil.cpp#L590-L603
[mesa-write]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L59-L68
[mesa-wrapper]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.h#L187-L215
[mesa-embedding]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf.h#L262-L295
[mesa-selection]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L109-L126
[mesa-gang-reset]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L2060-L2085
[mesa-queue-gang]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1436-L1457
[mesa-depth]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L5431-L5531
[mesa-dcc]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L5609-L5666
[mesa-color]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L5698-L5729
[mesa-image-init]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15944-L16008
[mesa-transition]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16131-L16175
[mesa-image-addresses]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_image.h#L308-L365
[mesa-clear-consumer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L5572-L5602
[mesa-predicate-consumer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_fast_clear.c#L355-L429
[mesa-barrier]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16317-L16399
[mesa-command-storage]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L221-L268
[mesa-submission]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1100-L1144
[mesa-command-reset]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L513-L540
[pal-metadata]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1385-L1413
[pal-metadata-record]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9MaskRam.h#L411-L417
[pal-metadata-address]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Image.cpp#L2183-L2202
[pal-metadata-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L796-L806
[pal-metadata-consumer]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/gfx9/gfx9RsrcProcMgr.cpp#L3153-L3209
[pal-update-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L3559-L3577
[pal12-update]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L831-L902
[pal-embedded-limit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.h#L167-L172
[pal-copy-cap]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L450-L534
[pal12-copy-cap]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L388-L453
[pal-allocator-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdAllocator.h#L85-L103
[pal-allocator-validation]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L48-L84
[pal-embedded-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4444-L4458
[pal-embedded-root]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.cpp#L330-L378
[pal-embedded-return]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.cpp#L1024-L1074
[pal-root-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L457-L482
[pal-allocator-reuse]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L808-L866
[mesa-upload]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1512-L1615
[mesa-copy-select]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_buffer.c#L370-L390
[mesa-copy-loop]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L217-L231
[completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L469-L479
[mesa-zrange-consumer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L5084-L5108
[mesa-hiz-consumer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L6143-L6165
[pal-tracking-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdAllocator.h#L52-L63
[pal-tracking-selection]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L110-L117
[xgl-update]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/vk_cmdbuffer_transfer.cpp#L79-L93
[vulkan-update]: https://github.com/KhronosGroup/Vulkan-Docs/blob/01aaacd99480487bf63830959513c5ca8ceb996d/chapters/clears.adoc#L741-L819
[clr-embedded]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/pal/palvirtual.cpp#L910-L990
[pal-upload-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/cmdUploadRing.cpp#L236-L255
[pal-internal-allocator]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.cpp#L1541-L1579
[pal-internal-chunk]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L1014-L1019
[xgl-update-entry]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/entry.cpp#L520-L532
[xgl-update-recording]: https://github.com/GPUOpen-Drivers/xgl/blob/e9782eb33ce5e5e4ed2e339542a28c1b933624b4/icd/api/vk_cmdbuffer_transfer.cpp#L648-L665
