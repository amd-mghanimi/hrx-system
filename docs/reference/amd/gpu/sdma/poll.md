# SDMA memory dependencies

A memory poll delays following SDMA work until its comparison is satisfied.
The producer's completion protocol, visibility of the control word and payload
cache transitions are separate contracts. A shader-to-SDMA handoff needs the
shader's release before publishing completion, the matching SDMA acquire where
required, and storage that remains live through the dependent operation. The
[cache reference](cache.md) and [cross-engine recipes](../recipes/README.md)
describe those surrounding operations.

## Applicability and packet families

The sources distinguish a legacy SI packet, the classic 32-bit comparison,
and a newer 64-bit comparison. Compute ISA, physical SDMA IP, runtime template
and native submission transport identify different parts of the protocol.

| Source-selected form | Representation and actual selection |
| --- | --- |
| Linux SI native ring | Six DWORDs; `DMA_PACKET_POLL_REG_MEM` opcode `0xe` at bits 31:28. The mask precedes the reference. [SI emitter][linux-si] |
| Linux CIK and SDMA 2.4 through 7.1 native rings | Six DWORDs; opcode 8, suboperation 0; reference precedes mask. The pipeline wait observes the ring's previous native fence sequence. [Native family map](#linux-native-pipeline-waits) |
| ROCr `POLL_REGMEM` | Six DWORDs; ordinary dependencies and selected fan-out joins. The selected template controls the separate scope field. [Builder][rocr-poll], [factory][rocr-factory] |
| PAL GFX10 / GFX12 event waits | Respectively six and seven emitted DWORDs, with different policy fields. GFX12 includes a zero `GRBM_GFX_INDEX` word. [GFX10][pal-wait], [GFX12][pal12-wait] |
| ROCr `POLL_MEM_64B` | Eight DWORDs; opcode 8, suboperation 5. The fused-copy service predicate is ISA major 12 with minor at least 5, independently of template selection. [Predicate][rocr-service], [fields][poll64-layout] |

## Classic memory equality

ROCr's ordinary unscoped builder emits this six-DWORD `POLL_REGMEM` form. The
operand is a readable, DWORD-aligned memory word; addresses are byte
addresses. Register polling uses a separate address convention and selector.
[ROCr fields][rocr-layout], [builder][rocr-poll], [Linux memory/register
split][linux-poll], [PAL layout][pal-layout]

| DWORD | Field | ROCr memory-dependency value |
| --- | --- | --- |
| 0 | `op` bits 7:0; `sub_op` bits 15:8 | 8; 0. |
| 0 | `hdp_flush` bit 26; `func` bits 30:28; `mem_poll` bit 31 | 0; equality 3; 1. Other header fields remain zero. |
| 1–2 | `addr_31_0`; `addr_63_32` | Full low/high address of the aligned control word. |
| 3 | `value` | Exact 32-bit reference; ordinary completion dependencies wait for zero. |
| 4 | `mask` | `0xffffffff`, comparing the full word. |
| 5 | `interval` bits 15:0; `retry_count` bits 27:16 | 4; `0xfff`, the classic infinite-retry value. |
| 5 | ROCr `scope` bits 29:28; reserved bits 31:30 | Zero in the unscoped template; scope 3 in the scoped template. |

The base header reserves bits 25:16 and bit 27. The following overlays are
source-specific extensions or replacements, not interchangeable names for
those reserved positions:

| Source layout | Additional or changed fields |
| --- | --- |
| PAL GFX10 `gfx103Plus`; Linux SDMA 6 and 7.1 generated headers | DWORD 0 `cache_policy` bits 22:20 and `cpv` bit 24. PAL reserves DWORD 5 bits 31:28; Linux supplies no scope macro there. [PAL][pal-layout], [Linux 6][linux6-layout], [Linux 7.1][linux71-layout] |
| PAL GFX12 | DWORD 0 `virtual_die_id` bits 17:16, `domain` bit 18, `bridge` bit 19, `mall_policy` bits 23:22, `mode` bits 27:26, `func` bits 30:28 and `mem_poll` bit 31; bits 21:20 and 25:24 reserved. DWORD 1 stores address bits 31:2 and reserves 1:0; DWORDs 2–5 retain high address, reference, mask and interval/retry, with bits 31:28 of DWORD 5 reserved. DWORD 6 is `GRBM_GFX_INDEX`. [Complete layout][pal12-layout] |
| UMR decoder selected for SDMA IP 5/6 | Cache policy bits 22:20 and CPV bit 24 are displayed under their respective IP predicates; HDP is bit 26. CPV applies for major > 5 or major 5 with minor >= 2; policy also applies to major 4 with minor >= 4. [Selection][umr-selection], [fields][umr-poll] |
| UMR decoder selected for SDMA IP 7 | Policy bits 24:22, mode bits 27:26; the decoder displays five payload words after the header. This differs from Linux 7.1's policy bits and PAL GFX12's policy width and emitted extent. [Decoder][umr7-poll] |

PAL's GFX10 event builder can set read policy and CPV under its MALL support
and settings predicates. Its helpers combine a selected Navi2x bypass with
KMD read policy; CPV additionally depends on a nondefault setting and KMD
validity. The GFX12 builder selects the read temporal-hint setting when MALL
is supported, otherwise zero; the helper names values 0/1/2/3 as RT/NT/HT/LU.
Neither event caller establishes that all poll policy fields are zero.
[GFX10 policy][pal-policy], [GFX12 policy][pal12-policy]

ROCr and the classic Linux memory callers use interval 4. PAL uses `0xa` and
describes that setting as 160 clocks; Mesa selects the same named interval.
This does not establish a universal nanosecond period for interval 4. The
infinite-retry setting keeps valid asynchronous work independent of a
device-side deadline. These callers do not specify finite retry exhaustion's
error or completion behavior. [PAL wait][pal-wait], [Mesa wait][mesa-wait],
[Mesa interval definitions][mesa-interval]

### Legacy SI emission

SI's native memory-poll emitter uses the following six words. This is an
emitted representation, not a complete legal-field specification: in
particular, the source does not separately define the legal high-address
width or establish that its `0xff` operand means indefinite retry.
[Header construction][linux-si-layout], [memory and register callers][linux-si]

| DWORD | SI memory-poll value |
| --- | --- |
| 0 | Opcode `0xe` at bits 31:28; memory selector bit 27 set. |
| 1 | Low 32 bits of the native ring-fence byte address. |
| 2 | High address bits OR'ed with `0xff << 16`. |
| 3 | Mask `0xffffffff`. |
| 4 | Previous native `sync_seq` reference. |
| 5 | `(3 << 28) \| 0x20`: the emitter's equality selector 3 and interval operand `0x20`. |

SI's VM register poll omits the memory selector and uses an unshifted
register index. CIK instead uses register indices shifted by two, and its
memory form has the classic reference-before-mask order. A six-word extent
alone does not identify either packet's address or operand convention.
[CIK header and selectors][linux-cik-layout], [CIK memory and VM callers][linux-cik]

### 64-bit memory comparison

ROCr's `SDMA_PKT_POLL_MEM_64B_GFX1250` defines the comparison as
`FUNC(memory_data_64 & mask_64, reference_64)`. Its address is QWORD-aligned.
The builder clears the packet before assigning these values:
[Complete fields][poll64-layout], [builder][poll64-builder]

| DWORD | Field | ROCr builder value |
| --- | --- | --- |
| 0 | `op` bits 7:0; `sub_op` bits 15:8 | 8; 5. |
| 0 | `mtype` bits 17:16; `sys` bit 20; `snp` bit 22; `gpa` bit 23; `cache_policy` bits 26:24; `func` bits 30:28 | 0; 1; 0; 0; 0; equality 3. Bits 19:18, 21, 27 and 31 are reserved and zero. |
| 1–2 | `addr_31_3` in DWORD 1 bits 31:3; `addr_63_32` in DWORD 2 | Aligned byte address; DWORD 1 bits 2:0 are reserved and zero. |
| 3–4 | `reference_31_0`; `reference_63_32` | Complete 64-bit reference. |
| 5–6 | `mask_31_0`; `mask_63_32` | `0xffffffffffffffff`. |
| 7 | `retry_count` bits 23:16; `scope` bits 29:28 | Retry **0**, meaning infinite; scope 3 with `scopeFields`, otherwise zero. Bits 15:0, 27:24 and 31:30 are reserved and zero. |

There is no classic interval field, and the retry field is eight bits rather
than twelve. System-memory selection in the header and SYSTEM scope in the
last word are distinct operands. Copy and fence scope layouts do not establish
this packet's scope encoding. A 64-bit operand definition alone does not
specify atomic observation against every concurrent producer store width.

## Comparison selection

The classic `FUNCTION` field selects the relation to the reference value.
Linux's CIK definitions and AMD's UMR decoder name the same seven relations;
UMR names selector 7 `N/A`. [Linux names][linux-comparisons], [UMR
names][umr-comparisons]

| FUNCTION | Relation to the reference |
| --- | --- |
| 0 | Always passes. |
| 1 | Less than. |
| 2 | Less than or equal. |
| 3 | Equal. |
| 4 | Not equal. |
| 5 | Greater than or equal. |
| 6 | Greater than. |

Always-pass has no unsatisfied comparison state and cannot wait for a
producer's future control update. The names alone define neither signed
relational ordering nor wrap-aware completion counters. The ordinary ROCr,
PAL and Linux memory-equality callers use full masks. Mesa's common SDMA
builder accepts a comparison, reference and mask; its shared command-stream
wrapper selects comparisons 3, 4 or 5, and the actual gang waits use full-mask
greater-or-equal comparisons. Neither that wrapper nor the explicit formula
for the newer 64-bit packet supplies a general classic partial-mask contract.
[Mesa builder][mesa-wait], [wrapper][mesa-wrapper], [gang waits][mesa-progress]

Register partial masks in Linux serve driver-owned control protocols. CIK's
HDP operation selects register mode, equality and header bit 26; its low and
high address words instead name the response and request registers, each
shifted by two. Later register-write/wait helpers separately order a request
write and acknowledgment read. These sequences are distinct from polling a
client completion word. [CIK HDP][linux-cik-hdp], [register helper][linux-reg-wait]

## Signal lifetime and actual callers

### Ordinary ROCr dependencies

`SubmitCommand` samples each dependent 64-bit signal with `LoadRelaxed`.
Already-zero dependencies produce no poll. Otherwise it polls a nonzero high
word to zero before polling the low word. This relies on the signal producer
and reuse protocol; two 32-bit comparisons are not an atomic 64-bit timeline
observation. Applicable HDP maintenance and USER_GCR acquire follow the
dependencies before the copy body. The poll supplies neither operation.
[Sampling and selection][rocr-deps], [stream order][rocr-order]

The HSA async-copy contract requires system-coherent buffers, with producer
release and receiving-device acquire as applicable. Dependencies become ready
at zero; completion has its own update. The contract also disallows an
async-copy dependency on a future async-copy submission because the queue
arrangement can deadlock. It does not establish arbitrary consumer-first
progress between two SDMA queues. [Public copy contract][hsa-copy]

The hardware path writes signal addresses into commands; that does not retain
an external signal object for the caller. The ordinary host workaround instead
registers asynchronous signal handlers, whose runtime registration retains
and later releases the observed signal. That handler's relaxed observation is
not a general payload-acquire fence. Signal objects, values and referenced
payload therefore have distinct ownership and visibility obligations.
[External handle conversion][rocr-api-signals], [signal conversion][rocr-signal-convert],
[callback construction][rocr-callback], [handler registration][rocr-handler],
[handler removal][rocr-handler-release], [handler observation][rocr-handler-load]

### Fused-copy and fan-out dependencies

The 64-bit poll at the additional-dependency loop of
`SubmitLinearCopyBodyWaitSignal` belongs to fused **linear** copy. Its first
dependency is carried by the fused copy packet; separate 64-bit polls cover
additional dependencies. An indirect-address copy is a different operation.
The other selected 64-bit callers serve fused fan-out coordinator/worker
paths and multicast. [Linear caller][poll64-linear], [fan-out
composition](fanout.md), [indirect operands](indirect-copy.md)

| Selected wide-poll caller | Dependency and join behavior |
| --- | --- |
| Single fused linear copy | Poll every additional dependency for zero; dependency 0 is an embedded WAIT in each copy chunk. An empty dependency list omits that WAIT. [Caller and owner][rocr-linear-owner], [body][poll64-linear] |
| Fused coordinator with profiling | Poll sampled-nonzero user dependencies, then publish a private prologue signal. Body work waits on that private signal. A final output-equals-1 poll joins engine groups before the end sample and final zero store. [Coordinator][rocr-coordinator], [owner][rocr-fanout-owner] |
| Fused coordinator/workers without profiling | Poll additional user dependencies without host zero-elision; the first dependency is an embedded WAIT at the body's first entry. The coordinator joins output at 1 when it emits an epilogue. With no GCR, profiling or mailbox, it omits that epilogue and the last body decrement completes output. [Coordinator][rocr-coordinator], [workers][rocr-bodies] |
| Non-profiling multicast | Poll additional dependencies; dependency 0 is an embedded WAIT in every multicast chunk. The profiling route instead uses plain multicast through classic `SubmitCommand`. [Multicast][rocr-multicast], [owner selection][rocr-multicast-owner] |

The embedded WAIT is a block inside a copy packet, with its own function,
address, scope, reference and mask fields. It is not a standalone
`POLL_MEM_64B` packet and contains no classic retry/interval word. The
[fused wait/signal representation](fanout.md#fused-wait-copy-and-signal)
describes that boundary.

Classic fan-out uses a prologue to observe dependencies and publish a private
ready signal, worker bodies to perform payload operations and completion
updates, and an epilogue to join them. With platform atomics the epilogue
polls the output counter's low word for **1** before its final completion
update; without them it polls private body signals for zero. Those values
belong to that particular counter protocol, not an arbitrary output timeline.
The selected per-operation completion convention and its distinction from the
public batch API's shared-counter wording are detailed in the
[batch ownership contract](fanout.md#batch-composition-and-descriptor-ownership).
[Prologue][rocr-prologue], [epilogue][rocr-epilogue]

The prologue and epilogue functions also contain 64-bit branches, but their
fan-out owner selects those functions only on the non-`IsGfx125Plus` route.
Those branches do not establish additional active 64-bit services. Likewise,
the ordinary host-poll workaround is consumed by `SubmitCommand`; it is not
applied by the separate batch prologue/body emitters.
[Fan-out selection][rocr-fanout-owner], [ordinary workaround consumer][rocr-deps]

Private signal owners demonstrate why producer completion is not the last
use. Ordinary multi-engine copy initializes each peer signal to 2: peer
completion changes it to 1, the leader polls 1 and acknowledges it by
changing it to zero, and a zero-handler finally destroys the signal. Fan-out
instead retains private prologue/body signals until aggregate output reaches
zero. A prologue's zero permits bodies to begin; its cell remains live while
those bodies still contain waits. [Gang allocation and cleanup][rocr-gang],
[leader acknowledgment][rocr-gang-join], [fan-out cleanup][rocr-fanout-lifetime]

CLR's actual signal pool likewise waits both the current operation and the
following waiter before rearming a reusable entry. Its copy path separately
publishes prior GPU work with a release barrier and requests system scope
for following GPU work. A host helper's already-complete relaxed fast path
does not itself execute an acquire wait; that observation and the device's
scope-bearing barrier are distinct. [Pool reuse][clr-active],
[current/next waits][clr-waiters], [host wait helper][clr-wait-helper],
[copy dependencies][clr-copy], [release barrier][clr-release],
[AQL scope selection][clr-scope]

### PAL GPU events

PAL's SDMA `CmdBarrier` and `CmdAcquireEvent` call `WriteWaitEventSet` on
bound GPU-event storage. The event has a four-byte value with an eight-byte
allocation alignment: `0xdeadbeef` is set and `0xcafebabe` is reset. The wait
compares the full word against the set value. GPU set/reset emits an SDMA
`FENCE`; barrier processing, cache policy and overlap handling remain separate
from the comparison. [Barrier][pal-barrier], [acquire/release callers][pal-acquire],
[event values][pal-event-values], [allocation][pal-event-memory], [size/alignment][pal-event-storage],
[event completion stores](fence.md#pal-immediate-writes-and-events)

Binding a CPU-accessible event maps and initializes the slot; GPU-only events
leave initial state to the client. CPU status/set/reset use a volatile word
without an explicit payload acquire/release fence. Event destruction unmaps
the slot and does not wait for outstanding GPU users. A set observation thus
does not authorize resetting or freeing storage that another waiter can still
read. Native command completion remains the final-use owner for command
storage and outstanding event accesses. [Binding][pal-event-bind],
[GPU-only contract][pal-event-gpu-only], [CPU operations][pal-event-cpu],
[destruction][pal-event-destroy]

PAL's general `CmdWaitMemoryValue` contract describes CP execution. The base
implementation is unimplemented and the SDMA subclasses do not override it;
that public method is not the implementation of SDMA event waits.
[Public API][pal-memory-api], [base methods][pal-memory-base]

### Linux native pipeline waits

The selected Linux native ring emitters all read the previous
`ring->fence_drv.sync_seq` from the ring's own writeback address. CIK and later
rows emit classic equality, memory mode, full mask, interval 4 and retry
`0xfff`; optional policy fields remain unset. SI uses its distinct layout.

| Native backend | Memory-poll emitter |
| --- | --- |
| SI | [SI native pipeline][linux-si] |
| CIK | [CIK native pipeline][linux-cik] |
| SDMA 2.4 / 3.0 | [2.4][linux24-wait], [3.0][linux30-wait] |
| SDMA 4.0 / 4.4.2 | [4.0][linux40-wait], [4.4.2][linux-wait] |
| SDMA 5.0 / 5.2 | [5.0][linux50-wait], [5.2][linux52-wait] |
| SDMA 6.0 / 7.0 / 7.1 | [6.0][linux60-wait], [7.0][linux70-wait], [7.1][linux71-wait] |

`amdgpu_ib_schedule` selects pipeline synchronization from explicit-sync,
context-switch and VM requirements. It subsequently emits the new native
fence and, when requested, a separate 64-bit user fence. The earlier poll's
reference is neither that new native sequence nor the user-fence value.
Ring commit publishes commands; the CPU native-fence reader later observes
writeback and signals software fences. Its sequence processing does not grant
the hardware equality poll wrap-aware timeline semantics.
[Submission order][linux-ib], [native fence producer][linux-fence-producer],
[CPU observer][linux-fence-observer], [ring publication][linux-ring-commit]

IB backing is freed against the scheduler's finished fence, or the hardware
fence when no scheduler fence exists; the suballocator retains unsignaled
fences before reclaiming storage. The ring's writeback cell instead lives
with the ring and is drained during ordinary teardown. A passed poll is not
a new client-visible reclamation boundary. [IB ownership][linux-job-free],
[deferred suballocation][linux-suballoc], [ring drain][linux-ring-drain]

### Mesa progress and terminal joins

RADV's transfer queue uses an SDMA leader and can enlist an ACE follower for
operations SDMA cannot perform. Command-buffer progress uses two private
DWORDs: SDMA `FENCE` publishes leader progress to ACE, while a confirmed ACE
end-of-pipe write publishes follower progress to SDMA. Both waits compare the
full word with greater-or-equal. For the ACE-to-SDMA edge the producer also
selects generation-specific cache writeback; the SDMA poll is a separate
control dependency. [Producer and waits][mesa-progress],
[real image-copy handoffs][mesa-copy-handoff]

The progress allocation is eight bytes, aligned to four. RADV chooses a
separate VRAM BO for placement/coherency conditions and adds `GL2_BYPASS`
when the gang is noncoherent; otherwise it can use command-buffer upload
storage. Its coherence predicate names GFX12 through
`cp_sdma_ge_use_system_memory_scope`, not every later generation. Finalization
has each consumer clear the word it waited on, rearming command-buffer
progress state. Reset/destruction do not insert a GPU completion wait.
[Allocation and coherence][mesa-progress-allocation], [GFX12 flag][mesa-coherence],
[rearming][mesa-progress-finalize], [reset/destruction][mesa-command-lifetime]

A **different**, queue-persistent eight-byte BO and four pre/postamble streams
perform the terminal join. The leader writes a start value of 1; ACE waits
and clears it. ACE's postamble then publishes 1 in the other word with a
confirmed bottom-of-pipe store. The SDMA leader postamble polls for at least
1 and clears that word before leader completion. This join keeps outward
completion from permitting reuse while the follower still executes. Its
terminal ACE store has zero cache flags, distinct from the progress producer's
writeback choices. The queue BO uses `GL2_BYPASS` because these preambles run
before the main preamble's cache maintenance. [Complete terminal flow][mesa-join]

The native gang submission gives the leader scheduling dependencies on its
followers but exposes the leader's **finished** fence as outward completion.
The explicit terminal join supplies the device execution dependency; a
follower's scheduled fence alone does not. BO reservation fences also retain
native resources. They do not authorize the application to overwrite a
command buffer or control word before all its users finish.
[Native gang fence ownership][linux-gang], [queue submission][mesa-submit],
[PM4/SDMA ownership recipe](../recipes/pm4-sdma.md)

## Generation and transport distinctions

| Source-selected behavior | Exact condition and consequence |
| --- | --- |
| ROCr host polling workaround | ISA major 9, minor 0, stepping **not 10**. Ordinary `SubmitCommand` defers publication through host asynchronous handlers, citing premature poll completion. gfx942 is outside this predicate. Separate batch/fan-out dependency paths do not consume this workaround. The source gives no firmware cutoff. [Predicate][rocr-workaround], [consumer][rocr-deps] |
| ROCr scoped classic poll | `scopeFields` writes SYS scope 3 at DWORD 5 bits 29:28. The non-DXG factory selects V6 (`useGCR=false`, `scopeFields=true`) for ISA major 11 or 12 with minor at least 5. DXG selects V4 first. PAL's classic/GFX12 layouts reserve these bits, while Linux has no corresponding macro. [Factory][rocr-factory], [templates][rocr-templates], [builder][rocr-poll] |
| ROCr fused-copy predicate | ISA major 12 with minor at least 5 selects `IsGfx125Plus`, independently of the DXG/template choice. A conditional GCR branch inside a shared function does not imply a GCR command on the selected V6 route, where `useGCR=false`. [Initialization][rocr-service], [linear stream][poll64-linear] |
| PAL GFX12 memory-poll extent | The mode-0 event caller zeroes and emits all seven words including `GRBM_GFX_INDEX`; Linux SDMA 7.0/7.1 emits six. UMR's IP 7 display does not resolve whether the seventh word is conditional data, padding or a layout discrepancy. Zero contents alone cannot distinguish those interpretations. [PAL caller][pal12-wait], [Linux 7.1][linux71-wait], [UMR][umr7-poll] |

The policy-bit, scope and packet-extent disagreements above retain their
source and transport predicates. A compiler target name does not resolve a
physical SDMA revision or firmware parser contract.

### Adjacent poll operations

Generated headers also name the following opcode-8 forms. These definitions
supply field positions; they do not establish a complete ordinary user-queue
protocol or a result-storage retirement rule. All listed address halves and
value words are 32 bits unless a narrower field is stated.
[Linux adjacent layouts][linux-adjacent], [PAL verify layout][pal-verify]

| Suboperation | Generated representation |
| --- | --- |
| 1: `POLL_REG_WRITE_MEM` | Four DWORDs: header; source address bits 31:2 in DWORD 1; destination low/high in DWORDs 2–3. Linux SDMA 6/7.1 names header `cache_policy` bits 26:24 and `cpv` bit 28. |
| 2: `POLL_DBIT_WRITE_MEM` | Five DWORDs: header `ea` bits 17:16 plus the same policy/CPV positions; destination low/high in DWORDs 1–2; start-page address bits 31:4 in DWORD 3; page count in DWORD 4. |
| 3: `POLL_MEM_VERIFY` | Thirteen DWORDs: header mode bit 31, policy bits 26:24 and CPV bit 28 in the named variants; pattern in DWORD 1; compare-0 start/end pairs in DWORDs 2–5; compare-1 start/end pairs in DWORDs 6–9; record address in DWORDs 10–11; reserved DWORD 12. |
| 4: `VM_INVALIDATION` | Four DWORDs: header `gfx_eng_id` bits 20:16 and `mm_eng_id` bits 28:24; invalidation request in DWORD 1; range low in DWORD 2; acknowledgment bits 15:0 and range high bits 20:16 in DWORD 3. Linux uses this in driver-owned VM control after page-table writes. [Layout][linux-vm-layout], [native caller][linux-vm-caller] |

Some generated verify variants name the DWORD 4–5 pair `cmp1_end`, whereas
others name it `cmp0_end`. The declarations alone establish neither range
inclusivity nor verify-mode/result semantics. These forms cannot inherit the
ordinary equality builder's values or the 64-bit poll's memory policy.

## Programming sequence and lifetime

For a one-producer completion cell, the complete handoff is:

1. Establish native mappings and cache attributes for the cell, payload and
   command stream. Initialize the cell before publishing commands that use it.
2. The producer writes payload, performs its required release, and publishes
   the terminal control value.
3. The SDMA queue observes that value, performs the matching acquire where
   required, and consumes payload. Passing the comparison establishes readiness
   for that consumer, not completion of all independent users.
4. Join every final payload user, outstanding control waiter and command
   fetcher through the selected native completion protocol.
5. Rearm, overwrite, unmap or free each resource only after its own final user
   has completed. Completion cells and command storage can have different
   owners and different retirement frontiers.

A 64-bit signal represented by two classic DWORD polls additionally needs a
stable value/reuse protocol. Retaining the terminal value through all its
waiters prevents an old dependency from observing a later operation's initial
state. The native [copy-completion protocols](atomics.md),
[command-storage owners](command-buffers.md) and [ring publication
frontiers](publication.md) provide the surrounding operations.

[linux-si]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L428-L465
[rocr-poll]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2640-L2659
[rocr-factory]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L904
[pal-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L70-L104
[pal12-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L88-L119
[rocr-service]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L204-L207
[poll64-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L795-L871
[rocr-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L742-L793
[linux-poll]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L392-L415
[pal-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L2744-L2816
[linux6-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L4468-L4553
[linux71-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L4468-L4553
[pal12-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L2887-L2965
[umr-selection]: https://gitlab.freedesktop.org/tomstdenis/umr/-/blob/c18840f10c7d5a085f2aeb2bc0d33ab9b3b71709/src/lib/packet/sdma/sdma_decode_opcodes.c#L3027-3053
[umr-poll]: https://gitlab.freedesktop.org/tomstdenis/umr/-/blob/c18840f10c7d5a085f2aeb2bc0d33ab9b3b71709/src/lib/packet/sdma/sdma_decode_opcodes.c#L2109-2133
[umr7-poll]: https://gitlab.freedesktop.org/tomstdenis/umr/-/blob/c18840f10c7d5a085f2aeb2bc0d33ab9b3b71709/src/lib/packet/sdma/sdma_decode_opcodes.c#L2680-2703
[pal-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L388-L450
[pal12-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L346-L386
[mesa-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L47-L57
[mesa-interval]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L345-348
[linux-si-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sid.h#L557-L582
[linux-cik-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L495-L542
[linux-cik]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L822-L863
[poll64-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2662-L2687
[linux-comparisons]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cikd.h#L525-L541
[umr-comparisons]: https://gitlab.freedesktop.org/tomstdenis/umr/-/blob/c18840f10c7d5a085f2aeb2bc0d33ab9b3b71709/src/lib/packet/sdma/sdma_decode_opcodes.c#L44
[mesa-wrapper]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.h#L173-185
[mesa-progress]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1955-2037
[linux-cik-hdp]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L247-L264
[linux-reg-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1219-L1240
[rocr-deps]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L397-L442
[rocr-order]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L521-L567
[hsa-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2150
[rocr-api-signals]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L432-L518
[rocr-signal-convert]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/signal.h#L314-L341
[rocr-callback]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L308-L362
[rocr-handler]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L963-L985
[rocr-handler-release]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L2018-L2033
[rocr-handler-load]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L2146-L2157
[poll64-linear]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L666-L804
[rocr-linear-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1345-L1435
[rocr-coordinator]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1075-L1346
[rocr-fanout-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1706-L1898
[rocr-bodies]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1348-L1574
[rocr-multicast]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1697-L1856
[rocr-multicast-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1952-L1974
[rocr-prologue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L806-L940
[rocr-epilogue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L942-L1073
[rocr-gang]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1222-L1343
[rocr-gang-join]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L521-L650
[rocr-fanout-lifetime]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1868-L1895
[clr-active]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L747-L885
[clr-waiters]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L955-L994
[clr-wait-helper]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.hpp#L55-L97
[clr-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocblit.cpp#L486-L644
[clr-release]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2347-L2365
[clr-scope]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1624-L1633
[pal-barrier]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L242-L304
[pal-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L347-L442
[pal-event-values]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/gpuEvent.h#L54-L69
[pal-event-memory]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/gpuEvent.cpp#L156-L182
[pal-event-storage]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/gpuEvent.cpp#L35-L37
[pal-event-bind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/gpuEvent.cpp#L187-L244
[pal-event-gpu-only]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palGpuEvent.h#L54-L68
[pal-event-cpu]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/gpuEvent.cpp#L107-L151
[pal-event-destroy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/gpuEvent.cpp#L59-L75
[pal-memory-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4273-L4290
[pal-memory-base]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.h#L641-L660
[linux24-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v2_4.c#L757-L773
[linux30-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c#L1031-L1047
[linux40-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L1692-L1702
[linux-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L1286-L1295
[linux50-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c#L1261-L1277
[linux52-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_2.c#L1161-L1177
[linux60-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1150-L1166
[linux70-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L1169-L1184
[linux71-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1175-L1189
[linux-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L199-L349
[linux-fence-producer]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L101-L140
[linux-fence-observer]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L207-L247
[linux-ring-commit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L169-L189
[linux-job-free]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L296-L312
[linux-suballoc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/drm_suballoc.c#L455-L489
[linux-ring-drain]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L533-L560
[mesa-copy-handoff]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_copy.c#L106-138
[mesa-progress-allocation]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1832-1914
[mesa-coherence]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1213-1218
[mesa-progress-finalize]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L2060-2086
[mesa-command-lifetime]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1346-1485
[mesa-join]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1347-1492
[linux-gang]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_cs.c#L1294-L1376
[mesa-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c#L1752-1835
[rocr-workaround]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L262-L266
[rocr-templates]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L579-L590
[linux-adjacent]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L4557-L4788
[pal-verify]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L2763-L2885
[linux-vm-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L4044-L4102
[linux-vm-caller]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1178-L1203
