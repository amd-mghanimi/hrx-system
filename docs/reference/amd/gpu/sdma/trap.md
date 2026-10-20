# SDMA interrupts

`TRAP` requests a native interrupt from the SDMA command stream. Its context
value participates in the selected driver's notification protocol. A Linux
kernel-ring interrupt prompts inspection of that ring's fence writeback;
a KFD event notification instead identifies a process event with a pending
mailbox. The completion value, payload visibility and final storage use have
their own owners. [Native ring observer][kernel-observer] ·
[KFD event protocol](../notifications.md#gpu-notification-and-interrupt-decoding)

## Applicability

The packet definitions below come from public Linux, ROCr and PAL sources.
Their field widths describe those representations. The selected native
transport establishes interrupt routing, event identifiers and host wakeup
behavior. A declaration in PAL's generated GFX12 header does not widen the
KFD event namespace or its interrupt decoder.
[ROCr representation][rocr-layout] · [PAL GFX12 representation][pal12-layout]

Linux's older SI and CIK native fence emitters also have different command
tails. The [native emission table](fence.md#linux-native-emission) records
each backend's unconditional or flag-selected notification. Those native
ring callbacks do not define an arbitrary user-queue notification ABI.

## Packet representation

The two-DWORD `SDMA_PKT_TRAP` contains a header and an inline interrupt
context. A DWORD is 32 bits; this form occupies eight bytes. The context is
a value, not a GPU address. It contains no mailbox address, payload address,
PASID, fence value or cache-control operand.
[Complete ROCr definition][rocr-layout] · [PAL GFX10][pal10-layout] ·
[PAL GFX12][pal12-layout]

| DWORD | Bits | Native field | Representation |
| ---: | --- | --- | --- |
| 0 | 7:0 | `HEADER_UNION.op` / `SDMA_PKT_TRAP_HEADER_OP` | `SDMA_OP_TRAP = 6` (`0x06`). |
| 0 | 15:8 | `HEADER_UNION.sub_op` / `SDMA_PKT_TRAP_HEADER_SUB_OP` | Eight-bit subopcode; the selected ordinary builders emit zero. |
| 0 | 31:16 | `reserved_0` / unnamed bits | ROCr reserves and zeroes these bits; PAL leaves them unnamed. Linux defines no field here. |
| 1 | 27:0 | `INT_CONTEXT_UNION.int_ctx` / `int_context` | Interrupt context in ROCr, PAL GFX10 and the cited Linux header views. |
| 1 | 31:28 | `reserved_1` / unnamed bits | ROCr reserves and zeroes these bits; PAL GFX10 leaves them unnamed. Linux defines no field here. |
| 1 | 31:0 | `INT_CONTEXT_UNION.int_context` | The alternative PAL GFX12 declaration occupies the full DWORD. |

[Opcode][rocr-opcode] · [ROCr fields][rocr-layout] ·
[ROCr builder][rocr-builder] · [PAL GFX10 fields][pal10-layout] ·
[PAL GFX12 fields][pal12-layout]

| Source view | Context bits | Source operation |
| --- | ---: | --- |
| Linux Iceland and Tonga packet headers | 28 | Context at DWORD 1, shift 0, mask `0x0fffffff`. |
| Linux Vega10 and Navi10 packet headers | 28 | Same context offset, shift and mask. |
| Linux SDMA6.0 and SDMA7.1 packet headers | 28 | Same context offset, shift and mask. |
| ROCr `SDMA_PKT_TRAP` | 28 | `BuildTrapCommand` zeroes both words, sets opcode 6 and assigns the event ID to the bitfield. |
| PAL GFX10 `SDMA_PKT_TRAP` | 28 | Generated representation with four unnamed upper bits. |
| PAL GFX12 `SDMA_PKT_TRAP` | 32 | Generated representation with no upper reserved field. |

[Iceland][iceland-layout] · [Tonga][tonga-layout] · [Vega10][vega-layout] ·
[Navi10][navi-layout] · [SDMA6.0][sdma6-layout] · [SDMA7.1][sdma71-layout] ·
[ROCr builder][rocr-builder] · [PAL GFX10][pal10-layout] · [PAL GFX12][pal12-layout]

The inspected Linux and ROCr paths do not establish preservation of the
upper four context bits from the PAL GFX12 declaration through firmware and
the native interrupt consumer. The two-DWORD kernel-ring builders emit context
zero; ROCr's builder uses its 28-bit field even for the selected gfx125 copy paths.
KFD's SDMA decoders use 28 bits and its native signal page permits at most
4096 event slots. Packet width, decoder width and event allocation limits
therefore describe three different boundaries.
[Native emitters](fence.md#linux-native-emission) · [ROCr builder][rocr-builder] ·
[Decoder and event limits](../notifications.md#gpu-notification-and-interrupt-decoding)

### Older native command tails

The SI native callback emits one DWORD, `DMA_PACKET_TRAP = 7` in bits 31:28,
with all other macro arguments zero. CIK emits one DWORD with opcode 6 in
bits 7:0, subopcode zero and upper header argument zero. Neither callback
appends an interrupt-context word. These are the complete emitted tails;
they do not supply a nonzero-context encoding for those queues.
[SI macro][si-macro] · [SI emitter][si-emitter] ·
[CIK macro][cik-macro] · [CIK emitter][cik-emitter]

The later native callbacks emit the two-DWORD form with context zero.
Linux's discovery selector maps SDMA4.4.0 to the v4.0 backend and SDMA6.4.0
to v6.0; their notification behavior follows those selected callbacks.
[Native selection][native-selection] ·
[Complete backend comparison](fence.md#linux-native-emission)

### DUMMY_TRAP declarations

`SDMA_PKT_DUMMY_TRAP` has the same header/context shape in the cited
generated views, including the 28-bit PAL GFX10 versus 32-bit PAL GFX12
distinction. Its opcode is separate: Linux's Vega10 header declares
`SDMA_OP_DUMMY_TRAP = 16`, while Navi10, SDMA6.0 and SDMA7.1 declare 32.
Both cited PAL headers declare 32.
[Vega opcodes][vega-opcodes] · [Navi opcodes][navi-opcodes] ·
[SDMA6 opcodes][sdma6-opcodes] · [SDMA7.1 opcodes][sdma71-opcodes] ·
[PAL GFX10 declaration][pal10-dummy] · [PAL GFX12 declaration][pal12-dummy] ·
[PAL GFX10 opcodes][pal10-opcodes] · [PAL GFX12 opcodes][pal12-opcodes]

The ordinary notification builders described here select `TRAP`. The
`DUMMY_TRAP` declarations alone supply no notification, transfer-drain or
command-retirement protocol, and do not establish an interchangeable NOP.
[ROCr selected builder][rocr-builder] · [Native selected tail][native-tail]

## Selected notification flows

ROCr's notification tail is a DWORD `FENCE(event_id)` to the signal's
mailbox, followed by `TRAP(event_id)`. Its ordinary, fused and grouped
copy paths use the same builder; their completion and cache operations
occur in their own enclosing protocols.

| Actual ROCr owner | Notification selection |
| --- | --- |
| Ordinary `SubmitCommand` | A nonzero event mailbox selects the tail after the output-value update. |
| `SubmitLinearCopyBodyWaitSignal` | `fused_notify` controls the epilogue; the actual gfx125 single-copy caller uses its default true value, and a mailbox adds the tail. |
| Classic fanout `SubmitEpilogue` | A mailbox selects the tail after the joined output update. |
| `SubmitFusedCoordinator` | A mailbox keeps the epilogue present and selects the tail after its joined completion path. |
| `SubmitLinearCopyMulticastCommand` | A mailbox selects the tail after the nonprofiled fused multicast; the profiled route uses `SubmitCommand`. |

[Ordinary][rocr-ordinary] · [Fused single copy][rocr-fused] ·
[Classic fanout][rocr-fanout] · [Fused coordinator][rocr-coordinator] ·
[Multicast][rocr-multicast] · [Grouped copy ownership](fanout.md)

The exact gfx125 selector is compiler ISA major 12 with minor at least 5.
The selected factory supplies a no-GCR template for those paths; GCR branches
inside a templated function are not evidence that the actual caller emits
them. Classic fanout selects `SubmitEpilogue`, while the gfx125 branch
selects the fused coordinator. Those choices change the enclosing protocol,
not the TRAP layout. [Factory][rocr-factory] · [Template aliases][rocr-aliases] ·
[ISA predicate][rocr-initialize] ·
[Single-copy caller][rocr-single-caller] · [Default argument][rocr-default] ·
[Fanout caller][rocr-fanout-caller]

The native signal mailbox occupies eight bytes, but the ROCr tail writes
one DWORD. KFD observes a pending slot by inequality with its all-ones
sentinel, clears the slot, updates event state and wakes waiters. This is
not a 64-bit atomic output-value update. PASID and native interrupt routing
identify the process; those inputs do not come from address fields in TRAP.
[Native mailbox and routing](../notifications.md#gpu-notification-and-interrupt-decoding)

Linux's two-DWORD native-ring emitters instead supply context zero. The native
interrupt handler identifies a ring and calls `amdgpu_fence_process`, which
reads that ring's fence writeback and advances software fences. It does not
interpret zero as an application event identifier. An interrupt can prompt
inspection without proving that a particular requested sequence is complete.
[Ring handler][kernel-observer] · [Fence observer][fence-observer] ·
[Native ring versus user fences](fence.md#linux-ring-completion-and-user-fences)

## Programming sequence and lifetime

A KFD event-backed completion connects these separate owners:

```text
native queue and event/mailbox allocation
  -> enclosing copy's payload, cache and output-value protocol
  -> mailbox FENCE(event_id)
  -> TRAP(event_id)
  -> native slot acknowledgment and waiter wakeup
  -> host value comparison and memory acquire
```

The enclosing copy protocol determines the payload's release, output
operation and their ordering. Publishing TRAP alone does not perform those
operations. A waiter rechecks its signal condition after wakeup; interrupt
delivery and a satisfied condition are distinct observations.
[Copy completion](atomics.md#copy-completion-through-add64) ·
[Sleeping wait](../notifications.md#sleeping-waits-and-the-check-to-sleep-race)

The output update precedes the mailbox/trap tail. Observing that output can
therefore precede the tail's final accesses. Mailbox backing and event identity
remain live through their final notification and observer uses. Signal storage
has its own last users, including later dependent operations. Native event
destruction does not drain the GPU queue. Command storage also
retains its independent queue/IB lifetime; interrupt arrival is not a general
permission to overwrite command or payload memory.
[Notification resource lifetime](../notifications.md#final-users-and-reuse) ·
[Command storage](command-buffers.md) · [Publication and reuse](publication.md)

Return to [SDMA](README.md) or the [primary source map](../../sources.md).

[rocr-layout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L969-L986
[rocr-opcode]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L52-L63
[rocr-builder]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2978-L2986
[pal10-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L3130-L3153
[pal12-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L3293-L3315
[pal10-dummy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L2177-L2200
[pal12-dummy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L2250-L2272
[rocr-ordinary]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L616-L652
[rocr-fused]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L668-L806
[rocr-fanout]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L944-L1075
[rocr-coordinator]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1077-L1348
[rocr-multicast]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1699-L1858
[rocr-factory]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L856-L908
[rocr-initialize]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L155-L282
[rocr-single-caller]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1349-L1439
[rocr-default]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L112-L122
[rocr-fanout-caller]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1547-L1902
[kernel-observer]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1613-L1640
[fence-observer]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L207-L249
[iceland-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/iceland_sdma_pkt_open.h#L2125-L2146
[tonga-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/tonga_sdma_pkt_open.h#L2198-L2219
[vega-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L2895-L2916
[navi-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L4283-L4304
[sdma6-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L5015-L5036
[sdma71-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L5015-L5036
[si-macro]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sid.h#L557-L582
[si-emitter]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L106-L126
[cik-macro]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L495-L513
[cik-emitter]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L278-L299
[native-selection]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2785-L2846
[vega-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L24-L44
[navi-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L24-L46
[sdma6-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L24-L46
[sdma71-opcodes]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L24-L46
[native-tail]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L356-L386
[pal10-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L43-L54
[pal12-opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L43-L54
[rocr-aliases]: https://github.com/ROCm/rocm-systems/blob/f9ba16bbe70e365b2f59b268e847bef19ad9db6e/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L579-L590
