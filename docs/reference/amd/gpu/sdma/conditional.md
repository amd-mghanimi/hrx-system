# SDMA conditional execution

`COND_EXE` uses a memory value to control a counted range of following SDMA
commands. PAL uses it to predicate copies; Linux uses the same packet in a
driver-owned submission envelope. The condition chooses whether work runs.
Producer readiness, payload visibility and final completion have separate
operations and owners. [PAL copy caller][pal-copy]
[Linux submission envelope][linux-submit]

## Applicability and representation

The Iceland, Tonga, Vega and Navi packet headers define a five-DWORD
`SDMA_PKT_COND_EXE`. Linux's SDMA6 and SDMA7.1 headers retain its address,
reference and count words. PAL has separate GFX10 and GFX12 definitions;
their policy and address spellings differ. These source families describe
representations, while the [native transport](../architectures.md) determines
which queue consumes them. [Iceland fields][fields24] [Tonga fields][fields3]
[Vega fields][fields4] [Navi fields][fields5]
[SDMA6 fields][fields6] [SDMA7.1 fields][fields71]
[PAL GFX10 fields][pal-fields] [PAL GFX12 fields][pal12-fields]

| Word | Bits | Field and meaning |
| --- | --- | --- |
| 0 | 7:0, 15:8 | Opcode 9, suboperation zero. Policy fields depend on the layout below; the builders leave other bits zero. |
| 1–2 | 31:0 each | Predicate GPU byte address, low then high DWORD. PAL GFX12 names word 1 bits 31:2 `addr_31_2` and requires four-byte alignment; its low two bits are reserved. The older layouts spell the whole low word `addr_31_0`. |
| 3 | 31:0 | `reference`. Both PAL builders and Linux's scheduled emitters write 1. |
| 4 | 13:0 | `exec_count`, the direct number of following command DWORDs controlled by the condition. Bits 31:14 are reserved. |

[PAL opcode][pal-opcode] [PAL GFX10 builder][pal-builder] [PAL GFX12 builder][pal12-builder]
[Linux SDMA6 builder][linux-builder]

The guarded count excludes the five-DWORD condition packet. It is neither
bytes nor count-minus-one: the representable maximum is `0x3fff` DWORDs,
or 65,532 command bytes. PAL reserves the condition, emits complete copy
packets, and patches their actual DWORD extent before committing the command
space. Its linear-copy loop guards each emitted chunk independently. Valid
packet boundaries and sufficient backing remain producer obligations;
the field's width supplies no permission to split a packet or overrun its
containing stream. [PAL count patch][pal-patch] [Copy chunking][pal-copy]

PAL's Boolean caller treats a zero sampled DWORD as false and a nonzero DWORD
as true, using reference 1. Linux's driver-owned condition uses only values
zero and one with that same reference. The definitions name a 32-bit reference
but do not specify a general comparison function for other reference values.
In particular, the packet has no comparison selector, mask, poll interval or
retry count. The Boolean callers therefore do not establish arbitrary equality,
masked comparison, signed ordering or a wait-until-ready protocol.
[Boolean caller][pal-setup] [Normalization commands][pal-normalize]
[Linux condition initialization][linux-slot] [Binary setter][linux-condition-setter]

### Policy fields

| Source layout | Header policy |
| --- | --- |
| Iceland, Tonga, Vega and Navi definitions | Only opcode and suboperation are named for this packet. |
| PAL `gfx103Plus`, Linux SDMA6 and SDMA7.1 | `cache_policy` in bits 26:24 and `cpv` in bit 28. |
| PAL GFX12 | `mall_policy` in bits 27:26. The remaining high header bits are reserved. |

The native Linux emitters write only opcode 9 into the header. PAL's older
builder fills policy only when the device reports MALL. Its source-read policy
comes from `GetCachePolicy(Gfx10SdmaBypassMallOnRead)`; CPV requires a
non-default bypass setting and valid KMD-provided L2 policy. The helper's
three-bit policy combines L2 selection with LLC no-allocation, and its MALL
bypass predicate specifically tests Navi2x. Those conditions belong to this
consumer, rather than following from the packet's name.
[Older policy owner][pal-policy]

PAL GFX12 uses `GetMallPolicy(true)`: the configured source MALL policy when
MALL is present, otherwise zero. Its values are 0 RT, 1 NT, 2 HT and 3 LU.
These two bits are not the older three-bit L2/LLC field. Neither policy
substitutes for a producer dependency or a payload acquire.
[GFX12 policy owner][pal12-policy]

## Boolean normalization and sampling

PAL advertises Boolean64 memory predication on its DMA engine and leaves
Boolean32 unadvertised, even though the primitive reads one DWORD. The
implementation builds the public 64-bit decision from two 32-bit samples;
the API capability describes that composition, not the primitive's read
width. [Older engine capabilities][pal-capabilities]
[GFX12 engine capabilities][pal12-capabilities]

`DmaCmdBuffer::CmdSetPredication` allocates two embedded DWORDs with one-DWORD
alignment. Only the first is its private decision word `D`. Recording on the
CPU initializes `D` to the complement of the requested polarity `p`. The
executing normalization sequence is:

```text
COND_EXE source_low,  reference=1, count=4
  FENCE D, p
COND_EXE source_high, reference=1, count=4
  FENCE D, p
... later copy guards read D ...
```

Each `FENCE` here is four DWORDs and writes the private decision, rather than
signaling whole-submission completion. A nonzero half sets `D` to `p`; two
zero halves leave its initialized complement. The resulting first-execution
truth table is:

| Source value | Positive polarity, `p=1` | Inverted polarity, `p=0` |
| --- | --- | --- |
| Both DWORDs zero | `D=0`, skip the guarded copy. | `D=1`, execute it. |
| Either DWORD nonzero | `D=1`, execute it. | `D=0`, skip it. |

[Shared setup and polarity][pal-setup] [Older normalization][pal-normalize]
[GFX12 normalization][pal12-normalize] [Older FENCE builder][pal-fence]
[GFX12 FENCE builder][pal12-fence]

The input is sampled during normalization. Each subsequent copy guard reads
`D`, not the original 64-bit value. Two reads are not an atomic 64-bit
snapshot: the source remains stable until both have finished. PAL's public
predication contract assigns its input dependency to
`PipelineStageFetchIndirectArgs` and `CoherIndirectArgs`; the operation does
not discover whether another engine is still producing that input.
[Public dependency contract][pal-api]

One raw guard occupies 20 command bytes. The two guard/FENCE pairs occupy
72 command bytes, plus PAL's eight-byte embedded allocation. These are storage
costs derived from packet sizes, not execution timings. Each guarded linear
copy chunk adds another 20-byte guard. [Normalization][pal-normalize]
[Copy caller][pal-copy]

### Replay and independent mutable state

The private decision is persistent embedded GPU memory. PAL enables a
separate CPU staging buffer only for command allocations, not embedded data;
the initial CPU write therefore modifies the backing that the GPU later
updates. The command buffer's `End` associates embedded allocations with its
root command chunk. Their public GPU-address lifetime extends to reset or
the next begin, subject to completion of their users.
[Allocation policy][pal-embedded-policy] [Mapped backing][pal-embedded-map]
[Root association][pal-record-end] [Public embedded lifetime][pal-embedded]

The DMA command buffer inherits the base no-op `PreSubmit`; its submit hook
increments command-stream use counts. Neither operation restores `D`.
Consequently, the recording-time initializer alone does not provide a new
complement value on each replay. After a nonzero source has written `p`, a
later all-zero source executes neither conditional FENCE and leaves that
previous value. This follows from the recorded sequence; it is not a claim
about every client or installed driver. A reusable normalization protocol
includes an ordered restoration of its own decision storage for each execution.
[Pre-submit hook][pal-pre-submit] [DMA submit hook][pal-dma-submit]
[Submission bookkeeping][pal-submit] [Setup][pal-setup]

Command-storage retirement and mutable-state ownership are separate. PAL's
unconditional postamble increments the root chunk's busy tracker when that
tracker exists; its [MEM_INCR protocol](atomics.md#mem_incr-and-command-allocator-retirement)
accounts for command and embedded-storage use. It does not isolate two
simultaneous executions that write the same decision word. A private word
remains owned through its final writer and all guards reading that value.
[Older postamble][pal-postamble] [GFX12 postamble][pal12-postamble]

## Programming sequence

A conditional transfer between a producer and a downstream consumer has these
distinct boundaries:

1. The command producer finishes the condition packet and complete guarded
   range, and publishes their backing through the selected
   [queue protocol](publication.md). Predicate and payload allocations are
   accessible to the participating engines.
2. The data producer publishes the predicate and selected payload, with a
   dependency and the directed [cache visibility](cache.md) contract. The
   SDMA consumer establishes readiness before sampling the predicate; a
   separate [memory poll](poll.md) can implement the control dependency.
3. SDMA samples the decision and executes or skips the counted body. Dependent
   copies inside that body retain their [transfer drains](ordering.md).
4. A completion operation outside the guarded body covers either outcome.
   The consumer observes that completion and acquires the selected result.
5. Command storage, predicates and payloads retire after their respective final
   users. A skipped body does not initialize its destination; the surrounding
   protocol defines the output associated with the false outcome.

PAL's actual copy paths insert guards around individual copies. Its fill and
timestamp methods do not use `WritePredicateCmd`, and the command postamble
is likewise unconditional. Predication in that API is not an ambient rule
that suppresses every later packet. [Copy][pal-copy] [Fill and timestamp][pal-unconditional]
[Postamble][pal-postamble]

## Driver-owned submission conditions

Linux's `sdma_v5_0`, `sdma_v5_2`, `sdma_v6_0`, `sdma_v7_0` and `sdma_v7_1`
backends provide `init_cond_exec`. The helper writes the same five-DWORD form,
reference 1 and a provisional zero count; the shared patcher computes the
following ring DWORD count modulo the ring size before publication.
[SDMA5.0][linux5] [SDMA5.2][linux52] [SDMA6][linux-builder]
[SDMA7.0][linux7] [SDMA7.1][linux71] [Ring count patch][linux-patch]

The condition points to a per-ring writeback slot initialized to CONTINUE,
value 1. This storage belongs to the native ring; it is not an application's
predicate allocation. `amdgpu_ib_schedule` starts its conditional extent
before launching the indirect buffers and patches it after emitting the
submission fence. Thus the native envelope can cover completion work too,
unlike a client copy guard followed by unconditional completion. Its meaning
depends on the driver's scheduling and preemption owner.
[Slot construction][linux-slot] [Submission extent][linux-submit]

For example, SDMA6's preemption owner clears the Boolean slot, emits and
commits a separate trailing fence outside those guarded submissions, and
requests native preemption. It waits for that trailing-fence value before
finishing the successful preemption sequence and restoring CONTINUE. This
observation belongs to the preemption handshake; it does not report successful
execution of the skipped payload or replace its original submission fence.
Changing the slot alone supplies neither that handshake nor final retirement.
[Binary condition setter][linux-condition-setter]
[Native preemption owner][linux-preemption]

The count spans the following outer-ring commands, including IB entry packets;
it does not add the lengths of the indirect bodies. The condition slot survives
across submissions and is released by `amdgpu_ring_fini`. Its writeback free
helper only returns the allocator slot; it supplies no per-predicate wait.
Final access is therefore the responsibility of the enclosing native ring
shutdown, independently of the last value written to the condition.
[Count calculation][linux-patch] [Ring teardown][linux-ring-fini]
[Writeback allocation and free][linux-writeback]

The same owner also wraps selected VM-flush work separately. These
driver-controlled uses establish a scheduled-ring protocol, not permission
for a client to modify the ring's condition slot or inherit its preemption
and retirement rules. The [command-buffer chapter](command-buffers.md)
describes the surrounding IB, CSA and submission ownership.
[VM-flush envelope][linux-vm]

[fields24]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/iceland_sdma_pkt_open.h#L1848-L1890
[fields3]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h#L1848-L1890
[fields4]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L2281-L2323
[fields5]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L3606-L3648
[fields6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L4269-L4323
[fields71]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L4269-L4323
[pal-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L185-L243
[pal12-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L170-L223
[pal-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L296-L322
[pal12-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L298-L320
[pal-patch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L258-L292
[pal-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L370-L450
[pal12-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L345-L385
[pal-capabilities]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L6789-L6809
[pal12-capabilities]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Device.cpp#L550-L565
[pal-setup]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1247-L1288
[pal-normalize]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L232-L256
[pal12-normalize]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L234-L258
[pal-fence]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L325-L353
[pal12-fence]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L323-L343
[pal-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L449-L481
[pal-unconditional]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1196-L1245
[pal-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4160-L4196
[pal-embedded-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L195-L230
[pal-embedded-map]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L123-L166
[pal-record-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.cpp#L346-L379
[pal-embedded]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4444-L4458
[pal-pre-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.h#L772-L776
[pal-dma-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.h#L225-L232
[pal-submit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/queue.cpp#L701-L737
[pal-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L176-L211
[pal12-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L183-L213
[linux5]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c#L302-L319
[linux52]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_2.c#L142-L159
[linux-builder]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L145-L162
[linux7]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L143-L158
[linux71]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L137-L154
[linux-patch]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.h#L525-L553
[linux-slot]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L285-L325
[linux-submit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L262-L350
[linux-vm]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_vm.c#L841-L912
[pal-opcode]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L32-L43
[linux-ring-fini]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L395-L419
[linux-writeback]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_wb.c#L61-L129
[linux-condition-setter]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.h#L482-L486
[linux-preemption]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1524-L1567
