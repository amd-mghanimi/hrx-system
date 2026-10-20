# PM4 pipeline-statistics queries

`SAMPLE_PIPELINESTAT` asks the command processor to write pipeline counters to
GPU memory. A query records beginning and ending samples, then resolves the
requested differences, including compute-shader invocations. Counter values,
query availability, and completion of later result readers are separate
observations.

These queries use `EVENT_WRITE` samples and source-specific result records.
[Global performance counters](counters.md) have their own event-selector,
instance and collection protocol. The query paths below do not establish that
protocol's ownership, or a general right to program privileged counter state.

## Packet representation

`IT_EVENT_WRITE` / `PKT3_EVENT_WRITE` is opcode `0x46`. A sample is four DWORDs
(16 bytes); a nonsample START or STOP event is two DWORDs (8 bytes). The type-3
header count is total DWORDs minus two. [PAL opcode][p9-opcode]
[GFX12 opcode][p12-opcode] [PAL ME/PFP header][p-headers]
[MEC header][p-mec-header] [GFX12 MEC header][p12-mec-header]
[Mesa header construction][m-header]

| Event | `event_type` | `event_index` | Total DWORDs |
| --- | --- | --- | --- |
| `PIPELINESTAT_START` | `0x19` | `0`, `other` | 2 |
| `PIPELINESTAT_STOP` | `0x1a` | `0`, `other` | 2 |
| `SAMPLE_PIPELINESTAT` | `0x1e` | `2`, `sample_pipelinestat` | 4 |

[Event values][p9-event-values] [GFX12 values][p12-event-values]
[Index selection][p9-index] [GFX12 index selection][p12-index]

The following fields cover PAL's merged `gfx9` and separate GFX12
`PM4_MEC_EVENT_WRITE`, `PM4_ME_EVENT_WRITE`, and `PM4_PFP_EVENT_WRITE` views.
The merged PFP form is explicitly GFX11-only. Mesa's GFX11/GFX12 schemas supply
corresponding engine views. The existence of a PFP declaration does not select
that engine for an ordinary compute queue.

| Word | Bits | Field and source scope |
| --- | --- | --- |
| 0 | 31:30 | Type `3`. |
| 0 | 29:16 | `count`: `2` for a sample; `0` for START/STOP. |
| 0 | 15:8 | `opcode = 0x46`. |
| 0, MEC | 7:0 | Reserved in the generated MEC headers. |
| 0, ME/PFP | 7:3; 2; 1; 0 | Reserved; `resetFilterCam`; `shaderType`; `predicate`. The ordinary sample and START/STOP callers below clear these bits. |
| 1 | 5:0 | `event_type`. |
| 1 | 7:6 | Reserved. |
| 1 | 11:8 | `event_index`. |
| 1, ME/PFP | 31:12 | Reserved. |
| 1, merged MEC base | 30:12; 31 | Reserved; `offload_enable`. |
| 1, GFX11 MEC overlay and GFX12 MEC | 28:12; 30:29; 31 | Reserved; `samp_plst_cntr_mode`; `offload_enable`. |
| 2, address view | 2:0; 31:3 | Reserved; `address_lo`. |
| 3, address view | 31:0 | `address_hi`. |

[Merged MEC][p9-mec] [Merged ME][p9-me] [GFX11 PFP][p9-pfp]
[GFX12 MEC][p12-mec] [GFX12 ME][p12-me] [GFX12 PFP][p12-pfp]
[Mesa GFX11 MEC][m11-mec] [ME][m11-me] [PFP][m11-pfp]
[Mesa GFX12 MEC][m12-mec] [ME][m12-me] [PFP][m12-pfp]

`samp_plst_cntr_mode` declares `legacy_mode=0`, `mixed_mode1=1`, `new_mode=2`,
and `mixed_mode3=3`. PAL's ordinary sample selects mode 0. Its GFX11/GFX12
ganged task sample selects mode 2; the [task-mode difference](#ganged-task-samples)
below keeps that choice separate from RADV's. PAL assigns these bits only for
compute on GFX11 in its merged builder, or compute in its GFX12 builder.
`offload_enable` is zero for these pipeline-statistics events; the builder's
offloaded `CS_PARTIAL_FLUSH` is another operation. [Merged builders][p9-build]
[GFX12 builders][p12-build]

ME/PFP also declare an alternate word-2 view: reserved `[2:0]`,
`counter_id[8:3]`, `stride[10:9]`, `instance_enable[26:11]`, and reserved
`[31:27]`. Mesa reserves word 3 in that alternate view; PAL uses its common
word-3 union. The pipeline-statistics callers select the address view, so the
alternate counter-selector fields do not describe their result-record layout.

Linux's `gfx_v12_1_pkt.h` separately names `EVENT_TYPE[5:0]`,
`EVENT_INDEX[11:8]`, `OFFLOAD_ENABLE[31]`, and the address operands. Its
`ADDRESS_LO(x)` macro places a 29-bit input in bits `[31:3]`. The macro group
does not name sampling-mode bits; that omission supplies neither a reserved-bit
rule nor a selected GC12.1 query mode. This GC12.1 view is not a GFX12.50
compiler-target predicate. [GC12.1 event macros][l121-event]

### Ordinary emitted words

For an eight-byte-aligned GPU byte address `A`, PAL's mode-0 builder and RADV's
pipeline sampler emit:

```text
DWORD 0 = 0xc0024600                 // type 3, count 2, EVENT_WRITE
DWORD 1 = 0x0000021e                 // SAMPLE_PIPELINESTAT, index 2
DWORD 2 = low32(A)                   // low three bits zero
DWORD 3 = high32(A)
```

PAL asserts eight-byte alignment, zero-initializes the packet, and inserts the
address words. Its mode-2 sample changes DWORD 1 to `0x4000021e`. START and
STOP emit header `0xc0004600` followed by `0x19` or `0x1a`, with no address
operand. These are the selected encodings; packet construction alone does not
establish queue admission or result-write atomicity. [PAL sample/nonsample
construction][p9-build] [GFX12 construction][p12-build]
[RADV sample][m-sampler] [Mesa nonsample emission][m-event-macro]

## Counter enable and sample addresses

Enabling counting, sampling a counter, and resetting query storage have
different owners. Ending a query need not stop the hardware counters: the
resolvers subtract its two recorded samples. The selected enable policies are:

| Caller | Enable and disable selection |
| --- | --- |
| PAL `gfx9` compute backend | The command-buffer preamble emits `PIPELINESTAT_START` unconditionally. `AddQuery` and `RemoveQuery` update active-query counts without emitting START/STOP. Explicit activation/deactivation emits START/STOP. |
| PAL GFX12 compute backend | The preamble emits START when `enablePreamblePipelineStats == 1`. The first added query activates counting when that setting is `0`. Removing a query leaves counters running; explicit deactivation emits STOP. |
| RADV MEC | Active-query changes request START/STOP flush bits. The lowerer implements those bits by writing `COMPUTE_PIPELINESTAT_ENABLE`, register byte address `0xb828`, bit `PIPELINESTAT_ENABLE[0]`, to `1` or `0`. |
| RADV graphics | The lowerer implements the same flush bits with START/STOP events. |

[PAL count tracking][p9-query-caller] [PAL preamble/activation][p9-query-policy]
[GFX12 query activation][p12-query-caller] [GFX12 preamble][p12-preamble]
[RADV active counts][m-active-counts] [GFX10-and-later lowerer][m-flush10]
[Earlier lowerer][m-flush-old] [Register address][m11-register]
[Register field][m11-register-field] [GFX12 address][m12-register]
[GFX12 field][m12-register-field]

The GFX10-and-later RADV lowerer identifies MEC by `AMD_IP_COMPUTE`; its
earlier lowerer also requires `gfx_level >= GFX7`. Dispatch preparation emits
the selected cache-flush state. RADV's aggregate counting state also includes
other query types, so these bits are not an exclusive pipeline-query lock.
PAL suppresses active queries around internal operations only when
`disableQueryInternalOps` is selected. RADV's meta begin/end path separately
suspends and resumes its queries. These are attribution policies implemented by
the drivers. [RADV dispatch preparation][m-dispatch]
[Aggregate active state][m-active-state] [PAL internal operations][p-save]
[RADV suspend/resume][m-meta] [RADV restoration][m-meta-end]

PAL's compute command buffer admits `QueryPoolType::PipelineStats`, and its
begin/end wrappers pass no hybrid stream. Both PAL's ordinary compute samples
and RADV's MEC samples address the `csInvocations` cell at **record base + 80
bytes**, with mode 0. PAL writes zeros into the other counter cells needed by
its resolver. A graphics sample uses the record base; separate ganged task
sampling has another address and mode policy. [PAL query predicate][p-compute-query]
[PAL begin][p9-begin] [GFX12 begin][p12-begin] [RADV begin/end][m-begin-end]

## Result records

The source layouts below are arrays of 64-bit values. Their width does not
establish an atomic GPU-to-CPU write or a hardware wrap period. The selected
resolvers compute differences and can truncate the output to 32 bits; that
arithmetic does not define the underlying counter's overflow behavior.

| Byte offset in one sample | Counter member |
| --- | --- |
| 0 | `psInvocations` |
| 8 | `cPrimitives` |
| 16 | `cInvocations` |
| 24 | `vsInvocations` |
| 32 | `gsInvocations` |
| 40 | `gsPrimitives` |
| 48 | `iaPrimitives` |
| 56 | `iaVertices` |
| 64 | `hsInvocations` |
| 72 | `dsInvocations` |
| 80 | `csInvocations` |
| 88 | `msInvocations` |
| 96 | `msPrimitives` |
| 104 | `tsInvocations` |
| 112 | PAL's additional `csInvocationsAce` |
| 120 | GFX12 PAL's additional `csInvocationsWgs` |

The names come from PAL's record types. RADV's API-statistic order maps to the
first fourteen positions through `pipeline_statistics_indices`; its compute
statistic maps to index 10, byte offset 80. The whole record is a software
container: the table does not imply that an ordinary MEC sample writes every
listed field. [PAL record][p9-record] [GFX12 record][p12-record]
[RADV counter mapping][m-record]

| Storage owner | One sample | One query's counter records | Separate availability storage |
| --- | --- | --- | --- |
| PAL `gfx9` pool | 120 bytes | Begin then end: 240 bytes | One four-byte marker per slot, after all counter pairs. |
| PAL GFX12 pool | 128 bytes | Begin then end: 256 bytes | Same separate four-byte marker array. |
| RADV below its `GFX10_3` predicate | 88 bytes, eleven QWORDs | Two samples; sixteen additional bytes when `uses_emulated_queries` is selected | One four-byte availability value per query, after all records. |
| RADV at/above its `GFX10_3` predicate | 112 bytes, fourteen QWORDs | Two samples; the same conditional sixteen-byte extension | Same separate availability array. |

For PAL pool base `P`, slot `s`, slot count `N`, and counter-pair size `B`,
the begin record is at `P + s*B`, the end record at `P + s*B + B/2`, and the
marker at `P + N*B + 4*s`. The bound allocation covers `N*(B+4)` bytes with
eight-byte alignment. CPU-accessible pools prefer cached GART; the other heap
preferences are separate. RADV allocates its query BO in GTT with 64-byte
alignment and maps it for host access. Those allocation choices do not change
the packet's eight-byte address alignment. [PAL storage requirements][p-pool]
[Bound allocation][p-bind] [PAL marker address][p-addresses] [RADV pool creation][m-pool]

For a pipeline-statistics pool, RADV selects the extra sixteen bytes when
`emulate_ngg_gs_query_pipeline_stat` accompanies the geometry-primitives flag,
or `emulate_mesh_shader_queries` accompanies the mesh-invocations flag. The
task-invocations flag independently selects `uses_ace`; it does not itself
request that storage extension. The fourteen-QWORD case also accommodates
emulated counters on GFX10.3. PAL's GFX12 WGS member is
distinct: `SampleWgsCsInvocationsCounter` writes zero, and the CPU resolver does
not add that cell to its result. A member name alone is not a native sample
operation. [RADV emulation predicates][m-pool]
[PAL WGS helper][p12-wgs] [PAL GFX12 CPU resolver][p12-cpu]

## Readiness and result processing

### PAL sentinel reads and marker waits

PAL resets each counter to `0xffffffffffffffff` and each marker to zero.
Its CPU `GetResults` maps the counter records when necessary and invokes the
counter resolver; it does not wait on the marker array. `IsQueryDataValid`
accepts a counter when either DWORD differs from the reset value. If the other
DWORD still equals `0xffffffff`, it executes an acquire-release C++ fence;
the caller then rechecks the full 64-bit begin and end values against the reset
sentinel. `QueryResultWait` repeats this algorithm until those checks pass.
The code explicitly describes the device write as non-atomic at the host.
That fence is an implementation step, not a standalone hardware guarantee
that two device writes have become one atomic sample. [CPU access][p-get-results]
[Sentinel algorithm][p9-cpu] [GFX12 algorithm][p12-cpu]

The CPU resolver subtracts begin from end for each selected counter and adds
the separate ACE CS delta for `QueryPipelineStatsCsInvocations`. It writes the
requested 32- or 64-bit result list when all requested data is ready or partial
results are requested, optionally appending availability. CPU accumulation is
a separate flag. Availability output belongs to this resolver's readiness
algorithm, not to a universal valid bit in every QWORD. [CPU results][p9-cpu]

PAL's GPU resolve takes a different path. End writes the immediate32 marker
`0xABCD1234` after the sample through `BuildReleaseMemGeneric`; GFX12 explicitly
selects `BOTTOM_OF_PIPE_TS`. The optional GPU wait uses 32-bit
`WAIT_REG_MEM`, equality and a full mask against that marker. The member name
`HasTimestamps()` denotes this readiness storage, not a clock sample.
[End and marker][p9-end] [GFX12 end][p12-end] [Marker wait][p9-wait]
[GFX12 marker wait][p12-wait]

The resolve builder then binds the selected built-in compute pipeline, query
source and output SRDs, and control/count/stride/statistic constants. Its
pipeline-statistics path asserts shader `noWait`: any requested wait is already
in the command stream. GPU accumulation is asserted unsupported. These host
builders establish the selected program and its inputs; they do not make the
CPU sentinel loop the specification of the generated resolve shader.
[PAL GPU resolve][p9-resolve] [GFX12 GPU resolve][p12-resolve]

### RADV availability and result shader

RADV resets records and availability to zero. End publishes a separate
immediate32 value `1` with write confirmation. The CPU checks that DWORD;
`VK_QUERY_RESULT_WAIT_BIT` on a GPU copy waits for it with 32-bit equality and
mask `0xffffffff`. With ACE and emulated mesh queries, the observers additionally
check the begin/end task-counter upper words containing `0x80000000`. Those
ready bits belong to that emulation protocol, not to every pipeline counter.
[RADV end and GPU waits][m-end-copy] [CPU observation][m-cpu]
[Ready-word comparisons](wait.md#waiting-on-a-ready-word-inside-a-wider-result)
[Release owner](release.md#mesa-events-queries-and-fine-fences)

The query-copy path requests `INV_L2 | INV_VCACHE`, adds framebuffer flush and
invalidation when WAIT is requested, and launches the result shader. The
shader checks availability, reads the selected 64-bit begin/end cells,
subtracts, and stores 32- or 64-bit results with optional availability. When
unavailable with PARTIAL, the shader writes zeros; the CPU result path instead
computes the recorded delta when PARTIAL is requested. These are source-selected
result policies. [Dispatch and cache work][m-query-dispatch]
[Result shader][m-result-shader] [CPU result path][m-cpu]

## Programming sequence and storage lifetime

One ordinary compute query has the following owners:

1. Allocate and map the complete counter/marker extent for its engine and
   observer. Keep the command stream, shader executable, query storage and
   eventual resolve destination valid for their GPU users.
2. Join any previous writers and result readers before resetting the slot.
   Reset initializes the backing records and availability; it is separate from
   enabling the counter state. Publish those writes through the selected
   [queue and command transport](publication.md).
3. Apply the selected enable policy and record the begin sample at the ordinary
   compute record's byte offset 80. Record the measured dispatches and the end
   sample at the corresponding end-record cell.
4. Publish and observe the source-specific readiness state. A GPU resolve has
   its own wait and [cache-acquire work](cache.md); a CPU read uses the pool's
   mapping and observer algorithm. Preserve the query record through the last
   resolve read.
5. Join the resolve and any subsequent output consumers before recycling their
   backing. Complete the actual command/native submission users before
   rebuilding command storage or freeing executable backing. Query availability
   is not those final-use observations.

PAL requires reset before a slot's next begin and keeps a query within one
command buffer. CPU reset maps and overwrites the records without joining GPU
users. GPU reset waits for prior query work on an engine supporting that query,
then fills the counters and markers; its other-engine path explicitly assigns
synchronization to client semaphores. The reset sequence does not establish
dependencies on arbitrary later or cross-queue readers. The query object
does not own the client's bound allocation, and its destructor supplies no
completion wait. [Public recording contract][p-api]
[PAL reset][p9-reset] [GFX12 reset][p12-reset]
[CPU reset implementation][p-cpu-reset] [Pool owner][p-pool]

RADV registers the query BO with the leader command stream and, when selected,
the ACE stream. Its `VkBuffer` query-copy wrapper also registers the destination
BO before delegating to the raw-address entry. Its GPU reset incorporates
`active_query_flush_bits`; the result-shader dispatch contributes
CS/cache work to that history so a later reset joins the reader. Pending reset
cache work is processed before subsequent query use. Host reset and pool
destruction do not wait for GPU users. [Begin/copy/reset owners][m-query-entries]
[Result-reader dependency][m-query-dispatch] [Host reset/destruction][m-host-owner]

| Backing | Final-use boundary |
| --- | --- |
| Query counter and marker storage | Last sample/fixup writer and last CPU or GPU result reader, including selected ACE users. |
| Copied-result destination | Resolve completion followed by the last application consumer of that output. |
| Command and embedded storage | The actual command/shader readers and the native completion or busy-tracking contract that retires them. |
| Resolve or measured shader code | Last execution that references the executable; copying a result or resetting a query does not retire code. |

[Command-storage modes and completed use](command-buffers.md#cpu-rebuild-after-completed-use)
explain PAL's tracked/client-owned modes. Its compute postamble drains CP DMA
and conditionally waits for shader readers before incrementing its tracker;
it relies on the scheduled KMD completion/cache trailer. A raw queue does not
inherit that trailer. [PAL postamble][p9-query-policy]
[GFX12 postamble][p12-preamble]

## Ganged task samples

PAL's GFX11/GFX12 task-statistic selection samples a separate ACE stream.
If that stream does not exist when the query begins, PAL defers the begin
sample. ACE initialization joins DE work before applying deferred queries,
because DE may have reset their slots. PAL's sampler selects mode 2 at
record offset 104; its comment describes that mode as writing only
`tsInvocations`. [GFX11 ACE sampler][p9-ganged]
[GFX12 ACE sampler][p12-ganged] [Deferred initialization][p9-ace-init]
[GFX12 initialization][p12-ace-init]

RADV sets `uses_ace` from the task-statistic flag. On its `gfx_level >= GFX11`
branch, ACE calls the ordinary pipeline sampler, leaving mode bits zero, at
the task-counter address. Earlier emulation uses a shader/GDS result copy and
separate high-word ready markers. PAL mode 2 and RADV mode 0 are distinct
selected encodings; the enum names and these callers do not establish that the
two modes produce an interchangeable counter population. [RADV selection][m-pool]
[Begin/end routes][m-begin-end] [Ordinary sampler][m-sampler]

The leader's query availability marker is separate from final gang retirement.
RADV submits the ACE stream with explicit pre/postambles: the ACE postamble
releases a semaphore and the leader waits and clears it before native leader
completion covers the gang. PAL's GFX12 postamble likewise joins its ACE work
before rearming the semaphore and completing command storage. The
[gang entry and final-join protocol](wait.md#gang-scheduling-preemptable-entry-and-final-join)
owns those wrappers and their reuse. [RADV gang wrappers][m-gang-wrappers]
[Native submission][m-submit] [PAL final ACE join][p12-postamble]

## GFX12 PAL no-gang address discrepancy

PAL's GFX12 `End` keeps `endQueryAddr` at the whole end record, then biases
`gpuAddr` by 80 bytes for an ordinary compute sample. Its compute wrapper
passes a null hybrid stream, so `End` subsequently passes that biased
`gpuAddr` to `FixupQueryForNoGangedAce`. The helper declares a whole-record
input and adds the TS offset before zeroing TS, ACE-CS and WGS-CS cells in the
end and begin records. The merged backend calls its corresponding helper only
inside the universal-engine branch. [GFX12 caller][p12-end]
[Helper's address contract][p12-no-gang] [Compute wrapper][p12-query-caller]
[Merged-backend predicate][p9-end]

For a GFX12 slot base `Q`, the counter pair spans `Q..Q+255`. The helper's
intended 24-byte writes address `Q+232..255` and `Q+104..127`; the biased
compute input instead selects `Q+312..335` and `Q+184..207`. The latter
targets the end-record IA-vertices/HS/DS cells, not the CS cell starting at
`Q+208`; the former is outside this slot's counter pair. The helper also selects
a universal-engine write description. These facts identify a source address
and caller-domain discrepancy, not a valid ordinary-compute programming recipe
or a demonstrated device outcome. The record base, CS sample address and
engine-appropriate fixup path remain distinct inputs.

[l121-event]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L303-L311
[m-active-counts]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L598-L609
[m-active-state]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.h#L681-L686
[m-begin-end]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L612-L757
[m-cpu]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2074-L2466
[m-dispatch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15321-L15385
[m-end-copy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L685-L800
[m-event-macro]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf.h#L372-L383
[m-flush-old]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L267-L528
[m-flush10]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L59-L264
[m-gang-wrappers]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1347-L1493
[m-header]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/sid.h#L272-L287
[m-host-owner]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1852-L1882
[m-meta-end]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta.c#L171-L263
[m-meta]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta.c#L18-L121
[m-pool]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1885-L2028
[m-query-dispatch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L1784-L1834
[m-query-entries]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2468-L2749
[m-record]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L394-L412
[m-result-shader]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L415-L595
[m-sampler]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L76-L123
[m-submit]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L1619-L1828
[m11-me]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L9134-L9287
[m11-mec]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L13729-L13846
[m11-pfp]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx11.json#L2947-L3100
[m11-register-field]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx11.json#L10712-L10716
[m11-register]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx11.json#L2120-L2125
[m12-me]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L9316-L9469
[m12-mec]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L13727-L13844
[m12-pfp]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/packets/cp_pm4_table_data_gfx12.json#L3037-L3190
[m12-register-field]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx12.json#L10629-L10633
[m12-register]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx12.json#L2070-L2075
[p-addresses]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/queryPool.cpp#L315-L359
[p-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L3959-L4022
[p-bind]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/queryPool.cpp#L214-L245
[p-compute-query]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/computeCmdBuffer.h#L55-L56
[p-cpu-reset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/queryPool.cpp#L264-L311
[p-get-results]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/queryPool.cpp#L114-L189
[p-headers]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L43-L57
[p-mec-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L43-L54
[p-pool]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/queryPool.cpp#L37-L111
[p-save]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L1220-L1329
[p12-ace-init]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L8480-L8520
[p12-begin]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineStatsQueryPool.cpp#L242-L299
[p12-build]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1437-L1588
[p12-cpu]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineStatsQueryPool.cpp#L560-L726
[p12-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineStatsQueryPool.cpp#L302-L372
[p12-event-values]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_enum.h#L8736-L8750
[p12-ganged]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineStatsQueryPool.cpp#L376-L394
[p12-index]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L56-L75
[p12-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_me_pm4_packets.h#L1687-L1764
[p12-mec-header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L43-L54
[p12-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1327-L1390
[p12-no-gang]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineStatsQueryPool.cpp#L203-L239
[p12-opcode]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_pm4_it_opcodes.h#L83-L90
[p12-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_pfp_pm4_packets.h#L2034-L2111
[p12-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L299-L386
[p12-preamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L929-L980
[p12-query-caller]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L357-L434
[p12-record]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineStatsQueryPool.cpp#L35-L126
[p12-reset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineStatsQueryPool.cpp#L469-L544
[p12-resolve]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/gfx12/gfx12RsrcProcMgr.cpp#L496-L627
[p12-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineStatsQueryPool.cpp#L423-L465
[p12-wgs]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12PipelineStatsQueryPool.cpp#L129-L157
[p9-ace-init]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9UniversalCmdBuffer.cpp#L11766-L11838
[p9-begin]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineStatsQueryPool.cpp#L266-L322
[p9-build]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L2377-L2581
[p9-cpu]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineStatsQueryPool.cpp#L537-L704
[p9-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineStatsQueryPool.cpp#L326-L397
[p9-event-values]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_enum.h#L14489-L14503
[p9-ganged]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineStatsQueryPool.cpp#L163-L194
[p9-index]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L60-L79
[p9-me]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L1665-L1747
[p9-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1479-L1551
[p9-opcode]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_pm4_it_opcodes.h#L76-L84
[p9-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L4208-L4289
[p9-query-caller]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L962-L1008
[p9-query-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1206-L1323
[p9-record]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineStatsQueryPool.cpp#L35-L121
[p9-reset]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineStatsQueryPool.cpp#L447-L521
[p9-resolve]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/gfx9/gfx9RsrcProcMgr.cpp#L507-L739
[p9-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PipelineStatsQueryPool.cpp#L401-L443
