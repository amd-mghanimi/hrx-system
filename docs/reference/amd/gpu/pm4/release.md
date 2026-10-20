# PM4 completion publication

`RELEASE_MEM` inserts a completion event into the GPU pipeline. The event can
perform selected cache actions, write a value or clock sample, and request a
notification. These are separate choices: a confirmed value write need not
interrupt the host, and publishing an event does not itself stop the following
command stream. A consumer joins the event through its memory value or the
applicable graphics-engine counter protocol.
[Release construction][p-builder] [Pipeline event and counter wait][m-pws]

## Applicability

The packet's generation, engine and native transport determine its form. PAL's
`gfx9` directory contains later GFX10/GFX11 definitions; that directory name
does not make every field applicable to physical GFX9. Its older PFP release
declaration is explicitly GFX11. GFX12 has separate MEC, ME and PFP declarations.
[Earlier MEC][p-mec] [ME][p-me] [PFP][p-pfp]
[GFX12 MEC][p12-mec] [ME][p12-me] [PFP][p12-pfp]

Mesa's common emitter provides an actual packet-selection predicate:

| Selected path | Packet and extent |
| --- | --- |
| `gfx_level >= GFX9` | `RELEASE_MEM`, eight DWORDs, type-3 count 6. |
| `GFX7 <= gfx_level < GFX9` and compute engine | `RELEASE_MEM`, seven DWORDs, count 5; there is no eighth interrupt-context word. |
| Remaining path, `CS_DONE` or `PS_DONE` after event adjustment | `EVENT_WRITE_EOS`, five DWORDs, count 3. |
| Remaining end-of-pipe path | `EVENT_WRITE_EOP`, six DWORDs, count 4; GFX7/GFX8 graphics additionally emit a preceding dummy EOP. |

[Common emitter][m-builder] [Legacy selection and workarounds](#legacy-completion-packets)

The native Linux GFX7/GFX8 compute fence emitters independently use the
seven-DWORD release. The GFX9, GFX9.4.3, GFX10, GFX11, GFX12.0 and GFX12.1
emitters use eight DWORDs. These are native callback selections, distinct from
a packet header's list of representable fields.
[GFX7][l7-compute] [GFX8][l8-compute] [GFX9][l9-fence]
[GFX9.4.3][l943-fence] [GFX10][l10-fence] [GFX11][l11-fence]
[GFX12.0][l12-fence] [GFX12.1][l121-fence]

## Representation

The following tables describe the eight-DWORD PAL forms. Word indices are
zero-based; PAL's `ordinal1` is word 0. The
[type-3 header](memory-commands.md#representation) uses opcode `0x49` and
count 6. An address is a byte address in the selected native GPU mapping;
the full field width does not establish the process VM's usable range.
[Opcode][l121-fields] [Packet fields][p-mec] [GFX12 fields][p12-mec]

| Word and native field | Representation |
| --- | --- |
| 1, `event_type` 5:0 | Pipeline event number; ordinary compute examples use `BOTTOM_OF_PIPE_TS` (`0x28`). |
| 1, `event_index` 11:8 | `end_of_pipe=5`, `shader_done=6`. Event selection and index must describe the same event class. |
| 1, `gcr_cntl` starting at 12 | Twelve bits in GFX10 overlays; thirteen in GFX11 and GFX12. [Release cache actions](cache.md#release_mem-cache-actions) gives each native generation's encoding. |
| 1, earlier `cache_policy` 26:25 | `LRU=0`, `STREAM=1`, `NOA=2`, `BYPASS=3`. |
| 1, GFX12 `temporal` 26:25 | `RT=0`, `NT=1`, `HT=2`, `LU=3`; these do not inherit the earlier policy meanings. |
| 2, `dst_sel` 17:16 | Destination selector below. |
| 2, `mes_intr_pipe` 21:20 | MES interrupt-pipe field. |
| 2, `mes_action_id` 23:22 | MES notification action below. |
| 2, `int_sel` 26:24 | Write-confirmation/interrupt mode below. |
| 2, `data_sel` 31:29 | Value source or operation below. |
| 3, `address_lo_32b` 31:2 | Low byte-address bits for the 32-bit view; bits 1:0 are reserved. |
| 3, `address_lo_64b` 31:3 | Alternative 64-bit view; bits 2:0 are reserved. |
| 4, `address_hi` | Byte-address bits 63:32. |
| 5–6, `data_lo`, `data_hi` | Immediate value words. The alternative `cmp_data_lo`, `cmp_data_hi` views name comparison operands. |
| 5, older `dw_offset` 15:0 and `num_dwords` 31:16 | GDS DWORD offset and count alternative. All three GFX12 forms omit this view. |
| 7, `int_ctxid` | Full 32-bit field in MEC; bits 27:0 in ME/PFP, with 31:28 reserved. |

[Earlier MEC fields][p-mec] [ME fields][p-me] [PFP fields][p-pfp]
[GFX12 MEC fields][p12-mec] [ME fields][p12-me] [PFP fields][p12-pfp]
[Event numbers][p-events]

The remaining word-1 controls distinguish the engines:

| Field | GFX10 MEC | GFX10 ME | GFX11 MEC | GFX11 ME/PFP | GFX12 MEC | GFX12 ME/PFP |
| --- | --- | --- | --- | --- | --- | --- |
| Bit 7 | Reserved | `wait_dma` | `wait_sync` | `wait_sync` | `wait_sync` | `wait_sync` |
| Bit 28 / bits 29:28 | `pq_exe_status` at 28 | `execute` at 28 | `pq_exe_status` at 28 | `execute` at 29:28 | `pq_exe_status` at 28 | `execute` at 29:28 |
| Bit 30 | Reserved | Reserved | `glk_inv` | `glk_inv` | `glk_inv` | `glk_inv` |
| Bit 31 | Reserved | Reserved | Reserved | `pws_enable` | Reserved | `pws_enable` |

[Earlier engine overlays][p-me] [MEC overlays][p-mec]
[GFX11 PFP][p-pfp] [GFX12 MEC][p12-mec] [GFX12 graphics forms][p12-me]

The other bits in these PAL views are reserved. In particular, all six PAL
forms reserve word 2 bits 28:27. The builders zero-initialize the packet and
write aligned low address words directly; the address-field view is not an
instruction to shift an already encoded byte-address word a second time.
[Earlier builder][p-builder] [GFX12 builder][p12-builder]

### Data and destination selectors

| `data_sel` | Earlier PAL declaration | PAL GFX12 declaration |
| --- | --- | --- |
| `0`, `none` | MEC, ME, GFX11 PFP. | All three engines. |
| `1`, `send_32_bit_low` | All three. | All three. |
| `2`, `send_64_bit_data` | All three. | All three. |
| `3`, `send_gpu_clock_counter` | All three. | All three. |
| `4`, `send_system_clock_counter` | All three. | All three. |
| `5`, `store_gds_data_to_memory` | All three. | Absent. |
| `6`, `send_emulated_sclk_counter__GFX11` | ME only. | Absent. |
| `7`, `atomic_add32bit_low__GFX103PLUS` | MEC only. | Absent. |

[Earlier selector sets][p-mec] [ME extension][p-me] [PFP set][p-pfp]
[GFX12 selector sets][p12-mec] [ME][p12-me] [PFP][p12-pfp]

The ordinary PAL builder selects TC/L2 destination `1`. Its other destination
enum values are memory controller `0`, queue write-pointer register `2`, and
queue write-pointer poll-mask bit `3`; the older header labels the last two
GFX11. Queue-control destinations require their native queue owner, and their
enum names alone do not define a client memory-publication protocol.
[Destination enums][p-mec] [Ordinary construction][p-builder]

The older generic builder explicitly excludes the GDS data selector. The
presence of selector `7` in the older MEC header does not specify its atomic
domain, return behavior or a complete ordinary caller. Likewise, a 64-bit
value selector establishes the operand width, not a general globally atomic
64-bit store contract. [Builder contract][p-builder]
[Atomic participant domains](atomics.md#participants-and-native-memory)

Clock selectors retain their [clock and sampling-point contracts](timing.md).
An event or query-readiness marker using immediate data is not a clock sample,
even when its source calls the destination a timestamp slot.
[Pipeline-query completion marker][p-query]

### Confirmation and notification selectors

| `int_sel` | PAL native spelling | Source-established distinction |
| --- | --- | --- |
| `0` | `none` | No interrupt requested. Selected callers can still write data; this is not `data_sel=0`. |
| `1` | `send_interrupt_only` | Interrupt-only name; Linux's older macro comment pairs it with `DATA_SEL=0`. |
| `2` | `send_interrupt_after_write_confirm` | Selected by native interrupting queue-fence emitters. |
| `3` | `send_data_and_write_confirm` | Selected by ordinary PAL data-writing releases without sending an interrupt. |
| `4` | `unconditionally_send_int_ctxid` | Declared context-notification mode. |
| `5` | `conditionally_send_int_ctxid_based_on_32_bit_compare` | Declared comparison mode; comparison signedness and complete native ownership are not supplied by the enum. |
| `6` | `conditionally_send_int_ctxid_based_on_64_bit_compare` | PAL's separate monitored-value interrupt helper selects this mode. |

[PAL modes][p-mec] [Ordinary confirmation][p-builder]
[Linux mode comment][l-nvd] [Native fence selection][l11-fence]

`BuildNativeFenceRaiseInterrupt` supplies a monitored address, comparison
value and interrupt context, with `data_sel=2` and `int_sel=6`. Its contract
explicitly says this helper does not write the value and must immediately
follow a release that does. The complete definition has no caller in the
pinned public PAL `src`/`inc` tree; it establishes a helper contract, not an
end-to-end native monitored-fence submission protocol.
[Monitored-value helper][p-native-interrupt]

`mes_action_id` separately names no notification `0`, interrupt and fence `1`,
interrupt without fence followed by address payload `2`, and interrupt with
address payload `3`. `mes_intr_pipe` identifies its pipe. Ordinary PAL
completion builders leave both fields zero. These fields do not make a
destination allocation into a host event; the native notification resource,
routing and observer remain separate owners.
[MES declarations][p-mec] [Builder defaults][p-builder]
[Native host notification](../notifications.md)

### Source-specific layouts

Mesa's six GFX11/GFX12 JSON objects agree on the eight-word extent and the
28-bit graphics versus 32-bit MEC context. They add distinctions not captured
by one shared C structure:

| Source definition | Difference |
| --- | --- |
| Mesa GFX11/GFX12 ME (`meg`) and MEC | Word 2 `add_doorbell_offset` is bit 28. PFP reserves bits 28:27; PAL reserves both in every listed engine. |
| Mesa GFX11/GFX12 PFP and ME | `execute=0` names `normal_fence_with_adjust`; `3` names `normal_fence_without_adjust`; `1` and `2` are reserved. MEC instead names `pq_exe_status=0` default and `1` phase update. |
| Mesa GFX11 ME | Data selector `6` is reserved, unlike PAL's emulated-SCLK declaration. |
| Mesa GFX11 MEC | Data selector `7` names atomic add32-low; PFP/ME reserve it. |
| Mesa GFX12, all three engines | Data selectors `5`, `6` and `7` are reserved, consistent with their absence from PAL GFX12 enums. |

[Mesa GFX11 PFP][m11-pfp] [ME][m11-me] [MEC][m11-mec]
[Mesa GFX12 PFP][m12-pfp] [ME][m12-me] [MEC][m12-mec]

Linux's separate GC12.1 header names word 2 bit 28
`PACKET3_RELEASE_MEM__ADD_DOOREBLL_OFFSET`, retaining that source spelling.
Its mode-5/6 names say `UNCONDITIONALLY_SEND_INT_CTXID_BASED_ON_*_COMPARE`,
where PAL and Mesa say `conditionally`. The inspected native fence emitter
selects mode `0` or `2`, so it does not resolve that naming disagreement.
The GC12.1 header also changes cache-action meanings inside word 1;
[the cache comparison](cache.md#release_mem-cache-actions) keeps those separate
from PAL GFX12. [GC12.1 fields][l121-fields] [Selected fence][l121-fence]

ROCr's separate `libhsakmt` `pm4_cmds.h` also contains eight-word MEC
declarations. Its `gfx9`/`gfx10` views reserve bit 7 and place
`pq_exe_status` at word 1 bit 29; its `gfx11` view moves that field to bit 28
and adds `glk_inv` at 30 but still reserves bit 7. Their word 2 bits 28:27
remain reserved. The associated `EndofKernelNotifyTemplate` declarations
are not the selected WDDM completion implementation described below.
[ROCr gfx9 declaration][r-def9] [gfx10][r-def10] [gfx11][r-def11]

## Execution and ordering

Event type chooses the producer stage. PAL's earlier generic compute builder
uses `BOTTOM_OF_PIPE_TS`, index 5. Its explanation says ACE treats a `CS_DONE`
release like an EOP release; graphics EOS has separate restrictions. PAL's
graphics-capable builders retain an EOP-only cache-action assertion and
restrict EOS data to immediate32/64. Mesa instead permits EOS cache actions
under its `gfx_level >= GFX12` predicate. These are distinct source contracts,
not a universal rule inferred from an event name.
[PAL event selection][p-builder] [GFX12 builder][p12-builder]
[Mesa predicate][m-builder]

PAL's compute immediate and event callers first resolve their stage masks.
Compute or bottom-of-pipe uses the release path; CP-stage immediate writes
use `COPY_DATA`, and CP-stage event writes use `WRITE_DATA`. The destination
is still live until the selected producer writes it and its consumers finish.
Placing a CP write after a dispatch does not turn it into shader completion.
[Earlier immediate][p-immediate] [Earlier event][p-event]
[GFX12 immediate][p12-immediate] [GFX12 event][p12-event]

CP DMA is a separate producer. Those callers request either a setting-qualified
`wait_sync` or an explicit DMA wait before publication. PAL restricts the older
combined feature to GFX11.0 with PFP firmware at least 2150, and disables its
GFX12 setting below firmware 2330; those restrictions are not unconditional
enablement. [The DMA chapter](dma.md#combined-release-wait-and-firmware-identity)
owns the complete setting and firmware predicates.

An event release can continue asynchronously with respect to following
commands. A `WAIT_REG_MEM` on its value supplies an explicit dependency;
the following acquire supplies any required consumer cache operations.
Graphics PWS uses an event counter and a matching `ACQUIRE_MEM` instead of
an ordinary application cell. PAL's EOP PWS path can use `data_sel=0`;
its EOS PWS path still supplies an owned dummy destination. Mesa's PWS emitter
uses zero destination/data fields even for its permitted EOS events. The
graphics counter protocol does not become a MEC field merely because the
opcode matches. [PAL split release][p-split] [GFX12 split release][p12-split]
[Mesa PWS construction][m-pws] [PWS acquire](cache.md#graphics-pws)

### A complete value dependency

PAL's compute `WriteWaitEop` supplies a concrete release-to-consumer sequence:

```text
owned control cell with a fresh expected value
  → preceding compute work and any separately required CP-DMA join
  → RELEASE_MEM BOTTOM_OF_PIPE_TS, immediate32 expected value,
      representable release cache actions, INT_SEL=3
  → WAIT_REG_MEM memory/equal, reference=expected, mask=0xffffffff
  → remaining ACQUIRE_MEM cache actions
  → dependent consumer work
  → final consumer and command-storage retirement
```

The helper obtains a private idle cell and incremented value. It partitions
cache work into the release, waits for that exact value, then performs any
remaining acquire. The equality wait establishes the dependency; the trailing
acquire is cache work, not another wait for the shader. Its cache partition
retains the generation-specific constraints described in
[complete barriers](cache.md#placement-in-complete-barriers).
[Earlier sequence][p-wait] [GFX12 sequence][p12-wait]
[Cell allocation][p-cells] [Value ownership][p-values]

For cross-queue payloads, [the handoff protocol](handoff.md) adds independently
published producer/consumer streams, a control mapping that the waiting engine
can observe before its payload acquire, and the terminal consumer's ownership.
Neither a ready value nor an interrupt alone proves all of those conditions.

## Selected publication and retirement owners

### Immediate values, events and timestamps

Immediate data is captured in the command packet; there is no borrowed host
pointer to that scalar. The destination, code, arguments, payload and command
bytes have separate final users. PAL attaches embedded/scratch allocations to
the command-stream root. With automatic reuse and busy tracking enabled, reuse
checks that root's GPU retirement state. Disabling busy tracking makes completed
GPU use a client precondition before returning chunks; without automatic reuse,
memory is recycled only through allocator reset. An internal release marker
does not replace the selected owner's completion proof.
[Immediate construction][p-immediate] [Root attachment][p-root]
[Root completion][p-root-completion] [Allocator reuse][p-reuse]
[Allocator modes][p-reuse-modes]

PAL's bound GPU event requires four bytes with eight-byte alignment. Its
CPU-accessible binding maps and resets the value; status reads the marker.
Destruction unmaps without a GPU wait. Set/reset, a queued GPU wait and all
later observers must finish before rearming, rebinding or freeing that cell.
The [event handoff](handoff.md) supplies payload availability/visibility
separately. [Event backing and host observation][p-event-owner]

GFX12 timestamp callers can select `noConfirmWr=true`, producing `int_sel=0`
while still writing a clock sample. PAL's timestamp barrier later confirms
the EOP and CP write paths through private scratch and joins their cache work.
The complete [timestamp sequence](timing.md#gfx12-timestamp-confirmation)
owns when the result can be read and reused. A later arbitrary release is not
a substitute for that sequence. [GFX12 sample caller][p12-timestamp]
[Deferred confirmation owner][p12-timestamp-sync]

CLR's PAL profiling owner illustrates the final host-side boundary: it waits
for the batch's last engine events before reading timestamp values, returns
their slots to the timestamp cache, and then releases the commands. The
sample operation, its visibility sequence and slot reuse are separate stages.
[CLR collection owner][r-clr-collect]

### Mesa events, queries and fine fences

RADV's event writer selects CP-stage `WRITE_DATA` or a later PS/CS/EOP event
from the source-stage mask, joining relevant CP DMA first. Its release uses
immediate32 and confirmation mode `3`. `CmdWaitEvents2` waits for value `1`
with a full mask before applying the supplied dependency barrier. On
GFX10–GFX11.7 its PS-stage classification deliberately excludes pre-raster
work: the source says `PS_DONE` does not join GS waves that send
`gs_alloc_req 0` there. [Stage selection][m-radv-event]
[Event wait and barrier][m-radv-event-wait]

The RADV event allocation is eight bytes with eight-byte alignment and
GL2-bypass placement; device-only events use VRAM, while host-visible events
use mapped GTT. GPU writes/waits use a DWORD; the host accessors use the
64-bit mapped slot. That allocation shape does not turn the GPU operation into
an atomic 64-bit publication. Destroying the event releases its BO without an
implicit wait for queued users. [Event owner][m-events]

Other actual callers select different observations:

| Caller | Publication and subsequent owner |
| --- | --- |
| RADV pipeline-statistics and GFX11+ mesh query end | Confirmed immediate32 availability marker, separate from the counter results. CPU result acquisition checks availability; GPU query copies and reset carry their own dependency/cache work. |
| RADV bottom-of-pipe timestamp | Confirmed clock sample; top-of-pipe uses `COPY_DATA`. |
| RadeonSI bottom fine fence | Immediate32 `0x80000000`, `int_sel=0`, into a zeroed four-byte cached-GTT allocation. Its observer tests nonzero. |
| RadeonSI timestamps and query end | Clock samples and separate readiness markers use `int_sel=0`; CPU result access uses synchronized BO mapping, and later GPU result readers have their own dependencies. |

[RADV query marker][m-query-pipeline] [Mesh marker][m-query-mesh]
[CPU query observation][m-query-cpu] [GPU copy owner][m-query-shader]
[Reset dependency][m-query-reset]
[RADV timestamps][m-radv-ts] [Fine-fence producer][m-si-fine]
[Fine-fence observer][m-si-signaled] [RadeonSI query end][m-si-query-stop]
[CPU result mapping][m-si-query-cpu]

RadeonSI can return from a fine-fence wait at an earlier point than whole-IB
completion. Its full path also accounts for deferred submission and native
fence waiting. Query storage recycling separately checks that the BO is no
longer referenced by the current command stream and that its native users
have finished. A ready marker is not the last reader of its backing.
[Fine-fence completion][m-si-fine-finish] [Query allocation/recycle][m-si-query-alloc]
[BO idle predicate][m-si-idle]

RADV's gang ACE postamble also uses a confirmed immediate32 EOP to publish
completion to its leader. The leader waits and clears that cell before native
leader completion can cover the joined gang. The
[gang protocol](wait.md#gang-scheduling-preemptable-entry-and-final-join)
connects this release to the separate entry handshake, submitted wrappers and
final command-storage users. [Postamble producer][m-queue-gang]
[Native gang submission][m-gang-submit]

### DRM user-queue trailers

Mesa's GFX/compute user-queue trailer writes a 64-bit progress sequence with
`RELEASE_MEM`, cache-flush/invalidate event, GL2 writeback, forward sequencing,
policy value `3` and `int_sel=0`. It then emits a separate
`PROTECTED_FENCE_SIGNAL`. Selection belongs to the winsys `userq_ip_mask`;
the protected signal's native resource is not an ordinary release destination.
The GFX12 policy value is temporal `LU`, not the GFX11 cache-bypass policy.
[Trailer packets][m-userq-packets] [Transport selection][m-userq-select]

The user-queue submission owner combines sync-object waits, VM progress and
shared-BO dependencies, associates BO read/write access with native signal
lists, and publishes the ring frontier after its memory fences. Its fence
observer first gates submission, then can observe the mapped sequence or wait
on the native sync object. The ring and mapped progress word share a
GL2-bypass GTT BO; native queue destruction precedes releasing that backing.
Progress observation, protected notification and ring capacity retain these
distinct owners. [Submission owner][m-userq-submit]
[Progress association][m-userq-fence-assignment] [Fence observer][m-winsys-fence]
[Queue backing lifetime][m-userq-storage]

### Native Linux queue fences

The native ring owns a CPU/GPU-mapped writeback slot and a 32-bit fence
sequence. `amdgpu_fence_emit` associates the next sequence with a software
`dma_fence` and requests the ring's fence callback with the interrupt flag.
The ordinary release backends listed above select `int_sel=2`. Its completion
processor samples the slot and signals matching software fences; interrupt
arrival itself does not replace the value check. The polling emitter omits
the interrupt. [Slot construction][l-slot] [Fence emission][l-emit]
[Completion observer][l-observe] [Polling emission][l-poll]

The listed release emitters choose immediate64 only when the 64-bit fence flag
is supplied, otherwise immediate32; they select `int_sel=2` for an interrupt
and `0` otherwise. Their eight-DWORD forms finish with a zero context word.
GFX10/GFX11 select policy value `3`; GFX12.0 retains the older macro spelling,
whereas GC12.1 explicitly spells its field `TEMPORAL(3)`. The GFX12 value must
not inherit the earlier meaning of cache bypass. Native release cache actions
also differ by generation. [GFX10][l10-fence] [GFX11][l11-fence]
[GFX12.0][l12-fence] [GC12.1][l121-fence]

The submission wrapper can place a 64-bit user-fence write before its native
32-bit fence, then append more ring-tail operations. Ring commit adds fetch
padding, executes a memory barrier and publishes the write pointer. Thus an
application-selected user value and the native submission's retirement fence
have different positions and owners. IB resource release uses the initialized
scheduler-finished fence, otherwise the initialized hardware fence, to defer
suballocation reuse. The [native-to-scheduler completion chain](../sdma/fence.md#linux-ring-completion-and-user-fences)
connects those owners; the long-lived ring/writeback allocation has its own
driver owner.
[Submission order][l-submit] [Ring publication][l-publish]
[Job resource release][l-job] [Deferred suballocation reuse][l-reuse]

KFD process events and DRM userq progress use different native resources and
interrupt routing. [Host notification](../notifications.md) and
[queue publication](publication.md) describe those transports. Their event or
doorbell identifiers cannot be inferred from the ordinary ring's zero
interrupt-context word.

### AQL carriers and WDDM translation

ROCr's `ExecutePM4` uses a special seven-word release under its ISA-major
`<= 8` branch. This is event index `7`, the AQL carrier-return operation, at
the end of a 64-byte ring slot. The source assigns it read-index advancement
and slot invalidation; it is not the ordinary event-index-5 value publication.
The caller observes read-index progress and then performs a host release store
to a supplied completion signal. Under ISA-major `>= 9`, it instead emits a
vendor AQL indirect-buffer packet and uses the AQL completion signal. The
host input bytes are copied, while the captured IB remains a device input
until its final use. [ROCr carrier and observer][r-execute]
[AQL carrier index][r-aql-index]
[Captured-command ownership](../aql/transfers.md)

The pinned WDDM AQL translator does not consume the declared
`EndofKernelNotifyTemplate` release structures. A translated dispatch with a
nonzero completion signal emits `BuildBarrier`'s `EVENT_WRITE` with
`CS_PARTIAL_FLUSH`, then cache acquire and, with platform atomic support, a
completion-signal decrement. A barrier packet with a completion signal also
gates `BuildBarrier` on `needs_barrier`; a processed vendor IB with a completion
signal emits that completion sequence after its captured commands. The default
declaration supplies `CS_PARTIAL_FLUSH` even where caller comments say
`CS_DONE`. [Dispatch translation][r-wddm-dispatch]
[Barrier translation][r-wddm-barrier-packet] [Processed vendor IB][r-wddm-vendor]
[Barrier defaults][r-wddm-default] [Barrier emitter][r-wddm-barrier]

Without platform atomic support, the submission worker waits on its native
sync-object timeline before decrementing the supplied completion signal on
the CPU; command-frame capacity waits on that timeline separately. These are
selected native translation paths, not an assumption that AQL completion
always means one release packet. [Native completion owner][r-wddm-process]

## Legacy completion packets

Mesa's common emitter keeps the older EOP/EOS encodings separate:

| Packet | Selected representation |
| --- | --- |
| `EVENT_WRITE_EOP`, opcode `0x47` | Six DWORDs. Word 1 contains event/index and cache flags; word 2 is low address; word 3 combines address-high bits 15:0 with destination at 16, interrupt mode at 24, and data selector at 29; words 4–5 contain data. |
| `EVENT_WRITE_EOS`, opcode `0x48` | Five DWORDs. Word 1 contains event/index; word 2 is low address; word 3 combines the 16-bit high address with EOS command at 29; word 4 supplies immediate data or the GDS operand. |

The common EOS immediate path selects memory destination and immediate32,
requires no cache flags, and uses EOS command `2`. EOS command `1` is the
separate GDS form; these command values are not the modern release data enum.
[Common legacy construction][m-builder] [Legacy field definitions][m-legacy]

The retained Evergreen/Cayman drivers provide real EOS export callers outside
the modern release-selector family. Evergreen command `0` saves one
append-count register; Cayman command `1` saves a GDS DWORD range. Both use a
40-bit destination in those emitters. Their atomic-buffer save owner then
publishes a separate incremented immediate32 EOS fence and waits for it
before consuming the saved state. The data export and its completion marker
are separate operations. [Evergreen export][m-eg-eos]
[Cayman export][m-cm-eos] [Export and fence owner][m-atomic-save]

The same opcode also appears in native backends with different emitted address
widths: amdgpu GFX6 masks EOP address-high to 16 bits, while radeon's SI fence
emitter masks it to eight. The latter publishes a 32-bit sequence and extends
observed progress in software. Neither source establishes a universal
64-bit-address EOP form or a 64-bit store merely because the software fence
counter is wide. [amdgpu GFX6][l6-fence] [Radeon SI][r-si-fence]
[Radeon sequence observer][r-observe]

Three source-selected adjustments explain why an isolated event packet is
not always the complete native sequence:

* Mesa replaces `CS_DONE`/`PS_DONE` with `BOTTOM_OF_PIPE_TS` under
  `gfx_level == GFX7`, attributing the restriction to CP-DMA TC destinations
  combined with EOS.
* Its GFX7/GFX8 graphics EOP path emits a dummy event to owned scratch before
  the real value. Linux's corresponding graphics fence callbacks emit a
  dummy sequence value followed by the requested value.
* Mesa's GFX9 non-MEC path, when supplied `eop_bug_va`, first emits
  `ZPASS_DONE` to scratch. RadeonSI's owner registers that scratch for the
  submission, with separate secure backing when needed; occlusion-query
  paths that already provide the event have a different caller predicate.

[Mesa adjustments][m-builder] [Scratch owner][m-scratch]
[Linux GFX7 graphics fence][l7-graphics] [Linux GFX8 graphics fence][l8-graphics]

These predicates belong to their complete source-selected paths. They do not
add dummy events to every modern compute release. Scratch remains live through
the event's write and submission retirement, just as the real completion
destination remains live through its final observer.

[p-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3335-L3538
[m-pws]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L179-233
[p-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1889-L2074
[p-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L2603-L2789
[p-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L4630-L4798
[p12-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1989-L2142
[p12-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L2467-L2626
[p12-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L3612-L3771
[m-builder]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L453-550
[l7-compute]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v7_0.c#L2160-L2178
[l8-compute]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c#L6142-L6161
[l9-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c#L5586-L5625
[l943-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L2985-L3017
[l10-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c#L8721-L8752
[l11-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L6076-L6107
[l12-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c#L4565-L4594
[l121-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L3738-L3769
[l121-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L312-L384
[p-events]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L37-L103
[p12-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1831-L1911
[p-query]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineStatsQueryPool.cpp#L326-L397
[l-nvd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/nvd.h#L354-L390
[p-native-interrupt]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3392-L3425
[m11-pfp]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L4939-5286
[m11-me]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L10255-10607
[m11-mec]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L14395-14741
[m12-pfp]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L5158-5493
[m12-me]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L10437-10777
[m12-mec]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L14625-14959
[p-immediate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L456-L516
[p-event]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1327-L1384
[p12-immediate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L259-L321
[p12-event]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L984-L1042
[p-split]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L1321-L1548
[p12-split]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L701-L892
[p-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1779-L1848
[p12-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1721-L1786
[p-cells]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L445-L498
[p-values]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.h#L528-L545
[p-root]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdBuffer.cpp#L333-L389
[p-root-completion]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdStreamAllocation.cpp#L457-L480
[p-reuse]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/cmdAllocator.cpp#L703-L779
[p-event-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/gpuEvent.cpp#L35-L245
[p12-timestamp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L198-L256
[p12-timestamp-sync]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L94-L119
[l-slot]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L407-L470
[l-emit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L101-L146
[l-observe]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L207-L249
[l-poll]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L159-L181
[l-submit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L294-L357
[l-publish]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ring.c#L169-L189
[l-job]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L296-L312
[l-reuse]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/drm_suballoc.c#L461-L488
[m-legacy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L147-178
[l6-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v6_0.c#L1901-L1927
[r-si-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/si.c#L3350-L3375
[r-observe]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/radeon/radeon_fence.c#L197-L257
[m-scratch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_fence.c#L38-123
[l7-graphics]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v7_0.c#L2114-L2147
[l8-graphics]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v8_0.c#L6046-L6083
[r-def9]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/impl/pm4_cmds.h#L659-L833
[r-def10]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/impl/pm4_cmds.h#L883-L1016
[r-def11]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/impl/pm4_cmds.h#L1026-L1098
[r-clr-collect]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/pal/palvirtual.cpp#L3580-L3660
[m-radv-event]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16417-16480
[m-radv-event-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16482-16526
[m-events]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_event.c#L19-129
[m-query-pipeline]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L684-757
[m-query-mesh]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1602-1634
[m-query-cpu]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2073-2466
[m-query-reset]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2561-2600
[m-radv-ts]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2751-2766
[m-si-fine]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_fence.c#L212-240
[m-si-signaled]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_fence.c#L201-210
[m-si-query-stop]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_query.c#L934-1031
[m-si-query-cpu]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_query.c#L1489-1523
[m-si-fine-finish]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_fence.c#L242-339
[m-si-query-alloc]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_query.c#L505-565
[m-si-idle]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/radeonsi/si_pipe.h#L2151-2156
[m-userq-packets]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L1488-1621
[m-userq-select]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L2352-2360
[m-userq-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L1623-1752
[m-userq-fence-assignment]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L2137-2163
[m-winsys-fence]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L182-243
[m-userq-storage]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_userq.c#L29-149
[r-execute]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1609-L1759
[r-aql-index]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_gpu_pm4.h#L87-L94
[r-wddm-default]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/include/impl/wddm/cmd_util.h#L50-L53
[r-wddm-barrier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/cmd_util.cpp#L46-L58
[r-wddm-dispatch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp#L661-L788
[r-wddm-process]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp#L1043-L1084
[m-eg-eos]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/evergreen_state.c#L5319-5347
[m-cm-eos]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/evergreen_state.c#L5349-5369
[m-atomic-save]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/drivers/r600/evergreen_state.c#L5516-5571
[m-query-shader]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1783-1834
[m-queue-gang]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1347-1500
[m-gang-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1718-1804
[r-wddm-barrier-packet]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp#L790-L877
[r-wddm-vendor]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/libhsakmt/src/dxg/wddm/queue.cpp#L879-L981
[p-reuse-modes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdAllocator.h#L48-L81
