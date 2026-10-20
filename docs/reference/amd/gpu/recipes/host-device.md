# CPU and GPU memory handoff

A CPU produces input, a GPU consumes it and writes a result, and the CPU
consumes that result. Each transfer needs both an execution dependency and
visibility through the consumer's memory path. Mapping the same allocation
into both address spaces establishes access; it does not establish either
handoff. Completion also has an owner: a shader's completion can retire its
arguments while a later copy or another queue still borrows its output.

The direct flow below uses host-accessible, fine-grained HSA memory and an
ordinary AQL dispatch. Native KFD/GEM mappings, PM4 barriers, CPU apertures and
staged transfers supply different implementations of the surrounding memory
edges. Their allocation and transport premises remain explicit.

## Mapping properties describe different things

| Property | What it determines |
| --- | --- |
| Physical placement | System DRAM, device VRAM/HBM, or backing that the native memory manager may migrate. |
| CPU mapping | Whether the CPU can address the allocation and whether accesses use cached, write-combining or uncached treatment. |
| GPU mapping | Access permissions, address translation, snooping and memory type, including whether GPU caches participate. |
| Observer scope | Which agents a release/acquire operation makes writes visible to; SYSTEM does not mean system-memory placement. |
| Control protocol | How a consumer recognizes the relevant completed producer and how the control value remains observable. |

Linux supplies a concrete example of the first three properties being
independent. KFD's owned GTT allocation selects the GTT domain; COHERENT,
EXT_COHERENT and UNCACHED separately become GEM flags. USERPTR registration is
a different allocation path. TTM chooses cached CPU backing unless
CPU_GTT_USWC requests write-combining. For GTT, it adds SYSTEM PTE treatment
and adds SNOOPED when the backing is cached. In the GMC11 mapping path,
COHERENT, EXT_COHERENT or UNCACHED selects GPU MTYPE_UC. A CPU-cached mapping
can therefore coexist with a GPU-uncached mapping. [KFD allocation][kfd-alloc]
[CPU cache selection][ttm-cache] [PTE attributes][ttm-pte]
[GMC11 memory type][gmc11]

The GEM COHERENT flag itself says that GPU instructions may still be needed
to flush caches at SYSTEM scope. UNCACHED describes use of GPU caches, not
the CPU mapping. Neither flag means that outstanding shader work has
completed. GC9.4.3/4 have a different PTE policy, including partition-sensitive
VRAM locality and UC non-VRAM mappings on the discrete device; the complete
predicates are in [staged GPU-local memory](local-memory.md#native-hbm-cache-policy).
[GEM flag contract][gem-flags] [GC9 policy][gmc9]

These native mappings are examples, not a translation of every HSA pool
allocation into one KFD allocation kind. ROCr reports pool grain, location and
per-agent accessibility separately; its system-region policy sets
`CachePolicy=CACHED` and independently sets `Uncached` for kernarg regions.
The latter does not replace the CPU cache-policy selection. A caller
uses the reported pool and access contract rather than inferring it from a
CPU pointer, a compiler target name or physical DRAM placement.
[ROCr region construction][region-flags] [Pool properties][pool-properties]
[Per-agent access][pool-access]

## Pool grain, agent access and SVM

Pool grain describes the memory-consistency contract for accesses to an
allocation. Direct access is a separate relationship between that pool and
each participating agent. HSA exposes these facts through different queries:

| Attribute | Query inputs | Meaning |
| --- | --- | --- |
| `HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS` | Global memory pool. | Grain and whether allocations permit kernarg initialization. |
| `HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS` | Requesting agent and memory pool. | Whether that agent can directly access allocations, and whether access requires an explicit grant. |
| `HSA_AMD_AGENT_INFO_SVM_DIRECT_HOST_ACCESS` | GPU agent. | Whether the host can directly access SVM memory physically resident in that GPU's local memory. |

[Pool flags][pool-flags] [Global-pool attribute][pool-global-attribute]
[Agent/pool attribute][pool-access-attribute] [SVM attribute][svm-host-access]

The access enumeration has three answers, all with the
`HSA_AMD_MEMORY_POOL_ACCESS_` prefix:

| Value | Answer | Direct-access contract |
| --- | --- | --- |
| 0 | `NEVER_ALLOWED` | The agent cannot directly access buffers in this pool; an access grant cannot establish this path. |
| 1 | `ALLOWED_BY_DEFAULT` | The agent can access buffers without an explicit grant. |
| 2 | `DISALLOWED_BY_DEFAULT` | The agent needs a successful `hsa_amd_agents_allow_access` call for the buffer before direct access. |

An access-set update includes every agent that must retain direct access;
the pool owner remains included. The pool-level
`HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL` reports whether every agent can
be granted access, not which particular pairs require grants or deny access.
[Access values][pool-access-values] [Access update][allow-access]
[Aggregate accessibility][pool-accessible-by-all]

The pinned ROCr `MemoryRegion::GetAccessInfo` applies the following ordered
policy. Later rows apply only when an earlier row did not return:

| Condition | Returned access |
| --- | --- |
| The requesting agent owns the pool. | `ALLOWED_BY_DEFAULT` |
| The link has fewer than one hop. | `NEVER_ALLOWED` |
| System pool, requesting CPU. | `ALLOWED_BY_DEFAULT` |
| System pool, other requesting agent. | `DISALLOWED_BY_DEFAULT` |
| Local pool with `fine_grain() == false`, including coarse and extended-scope fine grain. | `DISALLOWED_BY_DEFAULT` |
| Ordinary fine-grained local pool with equal requesting/owning `HiveId()` values. | `DISALLOWED_BY_DEFAULT` |
| Remaining cases. | `NEVER_ALLOWED` |

The hive comparison has no separate nonzero check. Pool exposure is another
predicate: ROCr makes ordinary fine-grained local pools user-visible when the
GPU's `HiveID` is nonzero or `HSA_FORCE_FINE_GRAIN_PCIE` is exactly `1`.
Exposure does not replace the per-agent query. Separately, ROCr answers the
SVM attribute from KFD's `CoherentHostAccess` capability; that query receives
neither a pool nor a requesting CPU agent. [Access implementation][pool-access]
[Pool exposure][pool-exposure] [Option parsing][fine-grain-pcie]
[SVM capability implementation][svm-host-capability]

The public header's access-attribute comment disagrees with these narrower
contracts: it says fine-grained pools cannot return `NEVER_ALLOWED`, and
requires an explicit grant for any pool not associated with the agent. The
access enumeration explicitly permits access without a grant for
`ALLOWED_BY_DEFAULT`; the implementation returns that value for CPUs accessing
system pools and can deny ordinary fine-grained local memory to another agent.
The returned agent/pool access value distinguishes these cases. Fine grain,
an exposed GPU pool or the SVM capability alone cannot establish the complete
CPU/GPU allocation contract. [Attribute comment][pool-access-attribute]
[Access values][pool-access-values] [Access implementation][pool-access]

## A complete fine-grained AQL flow

The participants in this flow are the host and one selected HSA GPU agent.
Input and output reside in host-accessible global memory whose pool reports
FINE_GRAINED. Kernargs use a region or pool that permits kernarg
initialization. The executable has already been loaded, made visible to
instruction fetch, and retained by its owner. A native HSA signal represents
completion; it is not an arbitrary payload word cast to a signal handle.
[Pool grain flags][pool-flags] [Signal representation][signal-create]
[Executable publication](../aql/dispatch.md#executable-publication-and-final-use)

1. Query the pool's allocation allowance, granule and alignment, and the
   host and selected GPU agents' access to it. Allocate input, output and
   kernarg backing.
   Where access must be enabled, `hsa_amd_agents_allow_access` names every
   agent that must retain direct access; the pool's owning agent remains
   included. The call is not an additive grant to an otherwise unknown set.
   [Allocation contract][pool-allocate] [Access update][allow-access]
2. Establish exclusive write ownership for this use. Fill input and arguments
   and initialize output as needed. Create a completion signal with value 1
   and a consumer set that includes its host waiter, or with the unrestricted
   consumer form. Keep the arguments unchanged until dispatch completion.
   [Signal consumers][signal-create] [HSA §3.3.3.1][hsa]
3. Prepare a dispatch packet with SYSTEM acquire and SYSTEM release. Reserve
   a writable ring slot, write its body while INVALID, then release-publish
   its valid first DWORD and notify the queue. Host writes must be published
   for their actual mappings and store instructions before that transition.
   [AQL publication](../aql/publication.md#packet-publication)
4. The packet processor performs the dispatch acquire before active shader
   execution. The shader reads input and writes output. The completion phase
   performs the selected release and finishes it before atomically
   decrementing the completion signal. [HSA §§2.9.1–2.9.2][hsa]
5. The host waits with `hsa_signal_wait_scacquire` and inspects the returned
   value. A returned zero establishes this single-dispatch completion; merely
   returning from the wait does not. The API permits a spurious return, and
   the condition must stay satisfied until all dependent waiters have
   observed it. Only after the successful acquire observation does the host
   read output. [Acquire-wait contract][signal-wait]
6. Before the next use, join every additional reader, writer and signal
   consumer. Then rearm the completion value and rewrite the corresponding
   payload or arguments. Reusing a ring slot additionally requires its
   read-index retirement. Freeing code, backing or the queue requires the
   final users of those owners to have ended, not just one convenient
   completion observation. [Packet and task lifetime](../aql/publication.md#completion-and-queue-lifetime)

There are two distinct input-visibility rules in this sequence. HSA requires
kernarg contents to be released to SYSTEM before dispatch, which the release
publication of the valid packet format can provide. The packet processor
makes those kernargs visible without requiring an acquire fence in the
packet *for kernarg loads*. The payload's global-memory acquire is a separate
edge. Publishing arguments is not a substitute for acquiring input data or
publishing executable code. [HSA §3.3.3.1][hsa]

HSA signal operations have the specified memory order at SYSTEM scope. That
property belongs to the signal operation; it does not grant the same atomic
capability to arbitrary payload addresses. Nor does a host/GPU fine-grained
allocation establish coherence for an additional DMA device or an agent that
has not been granted access. [HSA §2.5][hsa] [Access update][allow-access]

ROCr's ordinary compute blit is a real consumer of this ordering. It places
dependency barriers before a SYSTEM-acquire/SYSTEM-release dispatch,
release-publishes the packet header, and uses the dispatch signal for its
blocking completion wait. The blocking path checks the acquired result for
zero before returning success. This is runtime policy built on the HSA
contract, not a claim that every transfer is executed by a shader.
[Dependency construction][blit-deps] [Dispatch construction][blit-dispatch]
[Blocking completion][blit-wait]

## Resident CPU/GPU exchange

A dispatch acquire precedes entry into the active phase. Host writes
published later while the shader remains resident need a new synchronization
edge inside that shader. An HSA exchange uses signal operations or admitted
global-memory atomics whose scope covers both agents, with payload backing
that obeys that memory model. An HSA signal's SYSTEM-scope semantics belong
to its native signal object; an arbitrary payload word has its own atomicity
and mapping contract. [HSA §§2.5 and 3.3.8][hsa]

For one input slot, the CPU finishes payload writes and release-publishes
its ready generation. The shader acquires that generation before reading
payload, then release-publishes input credit after every shader reader has
finished. The CPU acquires that credit before rewriting the slot. For an
output slot, the shader joins its writers and release-publishes the result;
the CPU acquires and reads it, then release-publishes credit. The shader
acquires that credit before the next overwrite. This composes the two
directed handoffs into a reusable slot; the [pipeline
recipe](../../interop/pipelines.md#resident-execution-and-slot-generations)
also covers generation arithmetic, multiple readers and native drain.

The [shader memory chapter](../shader-memory.md) gives the per-family
release/acquire instructions and their native mapping assumptions. A fresh
control load and payload acquisition are separate obligations; invalidation
after a polling loop cannot repair a loop that never observes the update.
Its [scalar-memory discussion](../shader-memory.md#scalar-loads-and-per-access-cache-policy)
also explains why a uniform address does not make mutable resident data
dispatch-immutable. Returning input credit orders completed reads as well as
writes, and a publisher joins all contributing waves before its outward
release.

## CPU publication and device cache operations

A CPU publication operation orders the CPU's writes. A device acquire makes
the relevant device loads observe them. GPU uncached control storage can make
a marker observable without supplying the cache operations required for a
separately cached payload. Conversely, a payload invalidate placed after a
control wait cannot rescue a wait that keeps reading a stale control value.
The [cross-queue handoff](../pm4/handoff.md) makes that split explicit for PM4.

Host store ordering follows the actual CPU mapping and instruction sequence.
For example, ROCr's blit producer issues `SFENCE` when publishing to a device
ring on its selected PCIe-ordering path, before the atomic release header
store. Its native x86 doorbell path also has explicit store ordering. These
operations order CPU stores; they do not invalidate shader caches or wait for
a shader to finish. A plain release operation, a write-combining drain and a
GPU cache command have different responsibilities.
[Blit publication][blit-publish]
[Ring and doorbell stores](../aql/publication.md#ring-placement-and-x86-stores)

PAL's GFX9-family barrier implementation gives the cache direction a concrete
representation. It classifies CPU/memory accesses as bypassing GL2. A
transition from those clients to a GL2 client requests GL2 invalidate plus
writeback, with destination shader-cache invalidation where required. The
opposite transition requests GL2 writeback. Its planning also accounts for
execution stages; cache maintenance alone is not the execution join.
[Access classes][pal-clients] [Cache directions][pal-cache]
[PM4 execution and cache ordering](../pm4/cache.md)

For a raw PM4 flow, the CPU-visible completion must be downstream of the
relevant shader join and release, through control storage suitable for that
write and host observation. An ordinary CP store after a dispatch does not
alone establish shader completion. PAL illustrates the distinction by using
an EOP release for compute/bottom-of-pipe events and WRITE_DATA
for CP-stage events. A host acquire orders later CPU loads after the observed
marker; it does not retroactively flush the producer's dirty GPU cache lines.
[Stage-selected event emission][pal-event]

The native transport owns any additional submission trailer and completion
object. Scheduled DRM work and a direct user queue need not carry the same
trailer. Their [command-buffer completion](../pm4/command-buffers.md) and
[cache protocols](../pm4/cache.md) remain part of the flow even when payload
and control use the same physical allocation kind.

## Publishing prior stream work to another executor

A runtime stream can span a compute queue, a transfer engine and host-serviced
memory operations. A dependency passed between those executors must cover the
work being handed off. Reading the runtime's most recent signal is insufficient
when earlier dispatches have not yet been attached to that signal. The
operation producing completion also supplies the release scope needed by the
payload's next observer.

CLR's SVM submission path makes this boundary explicit. Before obtaining the
wait list for prefetch or discard, it calls
`releaseGpuMemoryFence(kSkipCpuWait)`. Pending dispatches, dirty fence state or
external dependencies cause that helper to submit a BARRIER_AND using SYSTEM
acquire and release scopes. The barrier receives a completion signal from the
queue tracker. The helper skips its final CPU completion wait; it still emits
the device dependency when needed. [CLR barrier header][clr-barrier-header]
[Prior-work publication][clr-release-fence] [Signal attachment][clr-barrier]

`WaitingSignal(HwQueueEngine::Unknown)` then obtains dependencies for the
native operation. It considers the tracked current signal and external
signals, appending positive-valued tracked dependencies or waiting for them on
the CPU according to a runtime setting. Separately added raw dependencies are
appended without that filter. Consequently, the skip-wait argument above is
not a promise that every surrounding path is host-nonblocking. The native
SVM APIs receive dependency signals, ranges and completion storage, but no
HIP stream from which they could reconstruct missing dispatch dependencies.
[Wait-list construction][clr-wait-list] [Prefetch contract][svm-prefetch]
[Discard contract][svm-discard]

| Operation | Dependency and completion flow at the cited CLR revision |
| --- | --- |
| One SVM prefetch | Under the HMM support predicate, publish prior work, obtain the wait list, initialize completion to 1 and pass both to `hsa_amd_svm_prefetch_async`. |
| A batch of SVM prefetches | Publish prior work once, give each prefetch the same wait list and initialize the shared completion count to the number of requests. Each accepted prefetch owns one completion decrement. |
| A batch of SVM discards | Publish prior work, pass the wait list to one `hsa_amd_svm_discard_batch_async` call and initialize its completion to 1 for the whole batch. |

[Prefetch callers][clr-prefetch] · [Discard caller][clr-discard]

ROCr's prefetch owner waits for each dependency to equal zero before invoking
the native SVM operation; its asynchronous handler supplies the completion
decrement. Its discard owner separately joins the supplied dependencies and
decrements completion once after visiting the ranges. Neither owner can order
a dispatch omitted from those dependencies. Signal storage and terminal values
remain stable through all their waiters, including consumers that have not yet
reached the dependency. [Prefetch owner][svm-prefetch-owner]
[Discard owner][svm-discard-owner] [Signal lifetime](../aql/barriers.md)

These are memory-management operations with their own admission rules. The
discard API requires XNACK and reserved, unregistered SVM address ranges; it
does not accept every GPU buffer. The callers request SYSTEM scope for
subsequent work, independently of the preceding-work dependency. This is CLR
runtime policy at `105dd4ff35798f95646353bc08f6c885416ae17e`, not an HDP flush
or an extension of SVM operations to other allocation classes.
[Discard admission][svm-discard-admission] [Range checks][svm-discard-ranges]
[Following-work scope][clr-prefetch]

### Prefetch completion and page residency

SVM prefetch in this ROCr path is a host-serviced memory-management operation.
The accepted callback waits for its dependencies, requests
`HSA_SVM_ATTR_PREFETCH_LOC` through the native KFD ioctl, and then updates its
completion signal. The callback owns a range and destination node, not a
user-submitted SDMA packet. Linux's migration implementation submits its own
copies and waits for their final DMA fence before finalizing the migrated
pages. API-level asynchrony therefore does not establish a device-only
execution path. [ROCr callback][svm-prefetch-owner]
[Native request][svm-native-request] [Migration copy wait][svm-copy-wait]
[Page finalization][svm-page-finalization]

Native request success and physical residency also differ. In Linux
`fe2ec83746e501645709761605c2464a44fd2929`,
`svm_range_best_prefetch_location` can select CPU memory for a GPU request.
Its conditions include `apu_prefer_gtt` and access by GPUs outside the
destination's XGMI hive. With XNACK enabled, that topology check uses the
ACCESS_IN_PLACE set; with XNACK disabled, it uses both ACCESS and
ACCESS_IN_PLACE. This is a placement decision for that range and process,
not a GPU-architecture-wide prohibition. [Placement selection][svm-placement]

The migration code permits only a subset of a range's pages to migrate.
`svm_range_trigger_migration` also returns zero after its GPU-directed
migration attempt even when that attempt returns an error; it records the
outcome separately in `migrated`. Its CPU-directed branch propagates the
migration result. The enclosing attribute operation can report later mapping
errors after earlier changes have taken effect. Its source explicitly
describes that state as partially completed, without a guaranteed rollback.
Consequently, a successful request is neither proof of whole-range GPU
residency nor a promise that residency remains fixed for subsequent work.
[Partial page migration][svm-page-finalization]
[Migration result handling][svm-migration-result]
[Attribute update and mapping][svm-attribute-update]
[Later eviction][svm-eviction]

Acceptance, callback completion and native success also have distinct
meanings. The prefetch header specifies a negative completion value on an
asynchronous error, while the cited callback asserts native success and then
performs its normal decrement. With assertions disabled, that path does not
translate a failing native result into the declared negative value. The
discard callback similarly warns on native-operation errors and still
decrements completion. This source discrepancy limits the error information
carried by those counters; the prior-work dependency does not repair it.
[Prefetch result contract][svm-prefetch] [Native result handling][svm-prefetch-owner]
[Discard result handling][svm-discard-owner]

The two ROCr discard operations have different contents-preservation
semantics. `hsa_amd_svm_discard_batch_async` optionally requests CPU prefetch
and then uses `MADV_FREE`. The combined
`hsa_amd_svm_discard_and_prefetch_batch_async` uses `MADV_DONTNEED` before
requesting GPU prefetch. Each owner performs one completion decrement for its
entire range list; the combined owner also only warns on native errors.
Neither is a preserving copy operation, and neither counter reports a
per-range outcome. [Discard owner][svm-discard-owner]
[Combined discard and prefetch][svm-discard-prefetch-owner]

### Accepted work and signal reuse

HIP's `hipMemPrefetchBatchAsync` validates and copies the range, size and
destination arrays into an owned command before enqueueing it. CLR later
submits those ranges as independent ROCr prefetch calls sharing a completion
signal. Up-front argument validation does not make that sequence one atomic
native admission: each prefetch constructs its own asynchronous operation,
and resource allocation remains fallible. [HIP admission][hip-prefetch-batch]
[Command storage][clr-prefetch-command] [CLR submissions][clr-prefetch]
[ROCr operation storage][svm-operation-storage]
[Resource failure translation][svm-api-errors]

An accepted prefetch keeps its completion update until its callback runs.
Changing the completion value does not cancel that callback: asynchronous
registration monitors the dependency signal and retains that monitored
signal, while the operation separately stores its completion handle. The
callback receives no HIP batch state through which it could observe a later
member's submission failure. [Operation storage][svm-operation-storage]
[Dependency registration][svm-handler-registration]
[Callback retirement][svm-handler-retirement]

At ROCm systems `105dd4ff35798f95646353bc08f6c885416ae17e`, CLR responds to
a prefetch submission error with `ResetCurrentSignal`, which writes zero to
the shared completion and moves its tracker back. That branch supplies no
join for earlier accepted members. If such members remain outstanding,
the written zero cannot establish their completion. The signal pool also has
object-reference and reuse checks. Those checks do not turn that store into
cancellation or prove the end of the accepted callbacks.
This is a discrepancy in the cited owner's failure path, not a hardware
retirement rule. [Batch submission failure][clr-prefetch]
[Tracker reset][clr-signal-reset] [Signal reuse checks][clr-signal-reuse]

A composed owner therefore needs distinct evidence for submission acceptance,
operation outcome and the final use of borrowed storage. Rejecting one member
leaves earlier accepted members' obligations intact. A terminal error value
can report failure without proving that other members sharing the signal
have stopped using it. The completion storage remains live through its last
writer and waiter, and the referenced ranges remain live through their final
native and device users. [Prefetch result contract][svm-prefetch]
[Accepted callback][svm-prefetch-owner]
[Signal lifetime](../aql/barriers.md#native-signal-storage-and-last-consumers)

### A visible control store still needs its producing payload

The same distinction appears in CLR's batch-memory blit. A control store can
be visible to another agent while an earlier dispatch's payload remains in
the producer's cache. `submitBatchMemoryOperation` requests SYSTEM scope
before launching the blit, so ordering covers the preceding stream work as
well as the control operation. [Batch caller][clr-batch-memory]

`addSystemScope()` marks the next packet for SYSTEM scope and invalidates the
runtime's remembered fence state. `adjustHeader()` consumes that request and
sets both acquire and release scopes on the dispatch header; the invalid
remembered state prevents its consecutive-SYSTEM optimization from removing
this transition. The ordinary AQL submission path applies that header before
publishing the packet. [Scope request][clr-scope-request]
[Header transformation][clr-scope-header] [Submission owner][clr-submit]

The batch blit uses one work-item, which walks the parameter array in order.
That ordering is local to the batch; it does not extend a store's shader
memory scope to earlier dispatches by itself. Parameter storage comes from
the ordinary kernarg pool or the graph's retained kernarg storage when
capturing packets. The control address and any payload remain borrowed until
their independent consumers finish. [Blit construction][clr-batch-blit]
[Sequential kernel][clr-batch-kernel]

For a native equivalent, the producer join and outward payload release
precede publication of the ready value, and the consumer's wait precedes
payload acquisition. The exact packet or shader sequence comes from the
participating memory paths. [Shader release and acquire](../shader-memory.md#global-release-and-acquire-sequences)
[PM4 handoff](../pm4/handoff.md) [Inbound RDMA visibility](../../interop/rdma.md)

## Mapping is not an execution handoff

PAL permits an allocation to remain CPU-mapped while GPU command buffers
reference it. Its Map/Unmap pair establishes or removes CPU access; it does
not assign simultaneous write ownership. [PAL mapping contract][pal-map]

RADV similarly maps a BO through the native GEM mmap interface and unmaps the
CPU view separately. Its advertised host-visible types include both cached
GTT and write-combined GTT, and both are HOST_COHERENT. Its mapped-memory
flush/invalidate entry points return success without cache work. That is an
implementation choice paired with those memory types, not a rule that every
Linux GPU mapping has coherent CPU caches. RADV's optional device-coherent
types separately request GL2_BYPASS, translated to GPU MTYPE_UC on GFX9 and
newer. [RADV memory types][radv-types] [CPU mapping][radv-map]
[Mapped-range operations][radv-ranges] [Device-coherent types][radv-coherent]
[GPU mapping][radv-pte]

Ordinary `hsa_amd_memory_lock` is another distinct contract. It pins an
existing host allocation and returns an agent address that can differ from
the host address. Overlapping locks can produce aliases that are not
necessarily coherent with each other, and the ordinary returned agent
pointer has coarse-grained access. Pinning therefore does not turn an
arbitrary host allocation into the fine-grained flow above. Unlock uses the
original host pointer while that allocation is still alive.
[Lock contract][memory-lock] [Unlock contract][memory-unlock]

ROCr's synchronous `Runtime::CopyMemory` handles this ownership explicitly:
it checks pointer ownership, locks a host range when needed for the selected
GPU, substitutes the returned GPU address, and keeps the lock through the
copy call before unlocking. Agent mapping and transfer completion are
different owners even when the temporary lock is hidden inside a synchronous
operation. [ROCr host-copy owner][runtime-copy]

## Local apertures and staged transfers

CPU-visible VRAM is an aperture path, not the same mapping as CPU-cached
system DRAM. Linux's native aperture-access helper orders its CPU writes
before an HDP flush; for reads it invalidates HDP before reading where that
operation applies. HDP maintenance covers this host path and does not replace
shader-cache release/acquire. [Aperture access][aperture]

Inbound writes from an external device have a separate producer-completion
boundary. HIP exposes a host HDP visibility operation, while the consumer's
execution dependency and cache acquisition remain explicit. Its support
queries, native mapping lifetime and observer scopes are described in
[inbound RDMA visibility](../../interop/rdma.md).

The architecture and transport predicates matter. Linux bypasses its ordinary
HDP flush and invalidate paths for an APU outside passthrough under
`CONFIG_X86_64`. Its CPU-connected XGMI exclusion is separate and is not
guarded by that CPU architecture check. HDP 4.4.0, 4.4.2 and 4.4.5 omit the
cited read-invalidate operation. These are driver-selected cases, not a general
inference from the words APU, coherent or Large BAR. [HDP exclusions][hdp-exclusions]
[Read-invalidate revisions][hdp-invalidate]

System-memory staging avoids CPU access through that aperture. The complete
flow is CPU publication of upload data → transfer completion → shader
acquire/work/release → readback transfer completion → host acquire and read.
The upload buffer remains borrowed until upload completes; the local output
remains borrowed through readback. [Staged GPU-local memory](local-memory.md)

ROCr's asynchronous-copy API requires system-level coherent source and
destination buffers and access by both specified agents. Its fence guidance
is explicitly “in general”: sender SYSTEM release before copying and
receiver SYSTEM acquire before consumption. Dependency signals reach zero
before copying; the completion signal is decremented afterward, and a
negative value reports an error. A source signal observed by a later copy
must therefore remain alive and stable through that consumer's dependency
processing. [Asynchronous-copy contract][async-copy]

The ordinary SDMA implementation puts dependency polls before its copy and
the completion update after it. When platform atomics are unavailable it
uses FENCE instead of WRITE, because ordinary copy and write packets can
overlap. Its HDP policy is separate: for ISA major at least 9, excluding
gfx10.1, support depends on a non-XGMI link to the first CPU agent, and the
enabled path emits HDP after dependency polls. That broader runtime policy
does not disappear merely because one particular copy uses staging.
[Completion choice][sdma-completion] [Submission order][sdma-submit]
[HDP predicate][sdma-hdp]

The copy API excludes dependencies on future async-copy submissions because
native queue placement can deadlock them. A visible control value and valid
wait encoding do not establish that their producer can run. The completion
value also has a different lifetime from later notification commands: ROCr
can place a mailbox FENCE and TRAP after the completion update. Its gang-copy
leader polls each participant signal before performing that signal's final
update, so its storage cannot be destroyed during the leader's last read.
[Copy dependency restriction][async-copy]
[Completion and notification][sdma-notification]
[Gang-signal final use][sdma-gang]

## Source consumption and copy completion

A staged upload has three distinct storage lifetimes: the original CPU input,
the staging range read by the transfer, and the GPU destination. Capturing the
input into independently owned staging ends the original source's borrow. The
staging range stays borrowed through the transfer; the destination stays
borrowed through its downstream consumers. A path that completes the capture
before API return permits early reuse of the original input without implying
destination readiness.

HIP's `hipMemcpySrcAccessOrder` enum describes these source-access policies:

| Value | Source-access semantics described by the enum |
| --- | --- |
| `hipMemcpySrcAccessOrderStream` (`1`) | Source access follows stream order; API return supplies no source-completion boundary. |
| `hipMemcpySrcAccessOrderDuringApiCall` (`2`) | Access may occur outside stream order, but all source accesses finish before API return. |
| `hipMemcpySrcAccessOrderAny` (`3`) | Access may occur outside stream order and after API return; this permits reordering without requiring it. |

[Access-order definition][hip-source-order]

At ROCm systems revision `105dd4ff35`, the public `hipMemcpyBatchAsync`
declaration marks its attribute-array parameters unsupported, while the
implementation accepts these three access-order values and translates them
into copy metadata. The enum describes the requested semantics; the public
declaration and accepted implementation paths disagree about attribute support.
[Public declaration][hip-batch-declaration]
[Attribute validation][hip-batch-validation] [Metadata translation][hip-copy-metadata]

The pageable host-to-device path implements `DuringApiCall` capture in both
dispatch modes. With queued dispatch, `EnqueueBatchCommands` copies input bytes
into vectors owned by `BatchWriteMemoryCommand` before enqueueing. With direct
dispatch, `Command::enqueue` calls the backend inline, and `WriteBufferBatch`
disables host pinning for this access order. `getBuffer` then supplies staging,
and CPU copies consume the original input before the API returns successfully.
Device work uses that staging afterward. Stream order alone does not protect
input that this path may capture before its preceding producer completes.
[Snapshot owner][hip-batch-enqueue] [Command storage][clr-write-command]
[Direct dispatch][clr-command-enqueue] [Staging selection][clr-batch-write]
[Buffer selection][clr-staging-buffer]

Registered host/device and device/device operands instead take
`BatchCopyMemoryCommand`. The same classification admits supported swap and
indirect modes and rejects them for pageable transfers. Its `copyBufferBatch`
path carries preceding stream dependencies to the engines and joins their
completion signals, but selects no source snapshot or CPU completion wait from
`srcAccessOrder`. The device-side join orders subsequent work; it does not
consume the current source on the API thread. Thus this implementation does
not establish the enum's early-retirement contract for mapped operands merely
by accepting `DuringApiCall`. [Operand classification][hip-batch-classification]
[Mapped copy path][clr-batch-copy] [Device completion join][clr-batch-copy-join]

For a caller-owned staging flow, the reusable boundaries are explicit:

1. The CPU obtains ready input and copies it into an available staging range.
   Completion of that CPU copy permits reuse of the original input.
2. The CPU publishes staging to the transfer engine with the required memory
   visibility and execution dependency.
3. Transfer completion ends the staging borrow and makes the destination ready
   for a dependent consumer with the matching acquire operation.
4. The final destination consumer completes before that destination is reused.

When a transfer reads registered input directly, there is no independent
snapshot to return that first credit: the original allocation remains the
device's source until it has finished reading it. An
[indirect copy](../sdma/indirect-copy.md) also has
address-slot lifetimes distinct from the payloads those slots select.

## Peer GPU handoff

A second GPU adds another agent, mapping and directed access path. HSA AGENT
scope covers one agent; synchronization with another agent requires the wider
matching scope. One physical package need not be one agent, and one GFX942
agent can contain several L2 caches. Scope therefore does not translate into
a count of packages or XCCs. LLVM's GFX942 memory model and the native
GC9.4.x memory mappings retain separate local/remote cache treatment.
[HSA scope instances][hsa] [GFX942 hierarchy][gfx942-model]
[Native memory policy](local-memory.md#native-hbm-cache-policy)

ROCr exposes pool access, cache-coherent links and 32/64-bit atomic link
properties separately. The access-set update lists all permitted agents plus
the pool owner. For its asynchronous copy, both named agents must directly
access both buffers at their current locations, and payloads must satisfy the
API's system-coherence contract. These are different premises from the
existence of an interconnect or shared handle. [Pool/link properties][peer-access]
[Access update][allow-access] [Copy contract][async-copy]

A direct peer flow releases the source, establishes the dependency, then
acquires at the receiver before reading peer payload. A copy-based flow
instead releases the source, satisfies the transfer dependency, copies and
reports completion; the receiver acquires before using the destination. The
source stays borrowed through every peer read, and the destination through
its consumers. The native queue's engine must also serve the directed route.
ROCr separates topology-based blit selection, destination-specific recommended
engines, and explicit-engine restrictions. Its preferred-engine masks have
target-specific CPU/GPU choices, while targets without dedicated xGMI engines
use a different selection rule. The [SDMA engine-selection
chapter](../sdma/engine-selection.md) traces those predicates and their native
queue classes. These routing rules supply no additional memory-scope guarantee.

The native attachment also determines whether peer access retains device-local
placement. AMDGPU's DMA-BUF path couples P2P eligibility to exporter power
ownership and placement, while KFD can share same-hive backing directly.
The [external-memory placement contract](../../interop/external-memory.md#peer-placement-and-exporter-power)
distinguishes those paths from engine selection and payload synchronization.

## Imported buffers and final use

Linux DMA-BUF supplies an explicit CPU cache-access boundary for mapped
imports: SYNC_START with the appropriate READ/WRITE direction precedes CPU
access, and SYNC_END follows it. These operations provide cache coherency,
not exclusion against concurrent devices. The application waits for prior
device use before beginning CPU access and publishes new device use after
ending it, using the importer's actual fence/dependency protocol.
[DMA-BUF CPU-access contract][dmabuf-cpu]

The exporter can perform substantial work at that boundary. AMDGPU's
begin-CPU-access implementation may move an unpinned, eligible BO to GTT
for CPU reads. The import path preserves selected coherence/cache flags
when the exporter uses AMDGPU's own operations; that is not a promise about
arbitrary exporters. Sharing a handle and addressability alone therefore
cannot extend the host/GPU coherent recipe to another device.
[CPU-access implementation][dmabuf-begin] [Import flag boundary][dmabuf-import]

| Storage | Final use that must end before rewrite, rearm or release |
| --- | --- |
| Input payload | Every shader or transfer still reading that version. |
| Output or intermediate payload | Its producer release and every downstream reader; host readback also needs the final host acquire. |
| Kernargs and executable | Every dispatch that can fetch them, including their required backing and prefetch extents. |
| Completion/dependency signal | Its producer update and every host/device waiter; a stable terminal value is part of the protocol. |
| Ring slots and indirect command storage | The command processor's retirement boundary, separately from resource completion. |
| CPU mapping, agent mapping and allocation | All users of that address or backing, followed by the owning native unmap/unlock/free operations. |

The single-dispatch sequence has one terminal result. A multi-queue or staged
graph may need several joins to close these lifetimes. A completed producer
does not revoke a consumer's borrow, and rearming a shared signal too early
can erase the very completion that consumer still needs to observe.
[AQL dependency lifetime](../aql/barriers.md)
[PM4 handoff ownership](../pm4/handoff.md#owners-and-a-complete-sequence)

[hsa]: https://hsafoundation.com/wp-content/uploads/2021/02/HSA-SysArch-1.2.pdf
[kfd-alloc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L1712-L1783
[ttm-cache]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1190-L1214
[ttm-pte]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L1432-L1476
[gmc11]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c#L468-L513
[gem-flags]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/drm/amdgpu_drm.h#L158-L175
[gmc9]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1102-L1159
[region-flags]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_memory_region.cpp#L68-L114
[pool-properties]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_memory_region.cpp#L309-L352
[pool-access]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_memory_region.cpp#L355-L401
[pool-flags]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L1830-L1857
[pool-global-attribute]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L1871-L1888
[pool-access-attribute]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2606-L2630
[pool-access-values]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2503-L2523
[pool-accessible-by-all]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L1922-L1928
[pool-exposure]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L532-L569
[fine-grain-pcie]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/util/flag.h#L237-L238
[svm-host-access]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L821-L826
[svm-host-capability]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2584-L2587
[signal-create]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L1382-L1437
[pool-allocate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2035-L2088
[allow-access]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2673-L2711
[signal-wait]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2042-L2086
[blit-deps]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L676-L706
[blit-dispatch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L887-L916
[blit-wait]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L624-L648
[blit-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L911-L929
[pal-clients]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L56-L68
[pal-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L324-L365
[pal-event]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1327-L1383
[pal-map]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palGpuMemory.h#L604-L635
[radv-types]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L531-L572
[radv-map]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c#L708-L774
[radv-ranges]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_device_memory.c#L346-L400
[radv-coherent]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L628-L648
[radv-pte]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c#L33-L48
[memory-lock]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2785-L2821
[memory-unlock]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2872-L2888
[runtime-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L594-L662
[aperture]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c#L827-L860
[hdp-exclusions]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_device.c#L6541-L6575
[hdp-invalidate]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/hdp_v4_0.c#L39-L53
[async-copy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2132
[sdma-completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L469-L479
[sdma-submit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L521-L634
[sdma-hdp]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L187-L202
[dmabuf-cpu]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/dma-buf.h#L26-L85
[dmabuf-begin]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_dma_buf.c#L280-L319
[dmabuf-import]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_dma_buf.c#L415-L446
[sdma-notification]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L614-L649
[sdma-gang]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L589-L612
[gfx942-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L11263-L11333
[peer-access]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2580-L2647
[clr-barrier-header]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L71-L74
[clr-release-fence]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2362-L2380
[clr-barrier]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2183-L2231
[clr-wait-list]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L888-L953
[svm-prefetch]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L4240-L4266
[svm-discard]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L4268-L4307
[clr-prefetch]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L3247-L3331
[clr-discard]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L3335-L3369
[svm-prefetch-owner]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L3865-L3905
[svm-discard-owner]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L4035-L4118
[svm-discard-admission]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L2224-L2248
[svm-discard-ranges]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L3965-L3990
[clr-batch-memory]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L4368-L4387
[clr-scope-request]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.hpp#L653-L656
[clr-scope-header]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1624-L1654
[clr-submit]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1523-L1574
[clr-batch-blit]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocblit.cpp#L3615-L3652
[clr-batch-kernel]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/blitcl.cpp#L230-L256
[svm-native-request]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/libhsakmt/src/svm.c#L38-L115
[svm-copy-wait]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_migrate.c#L184-L211
[svm-page-finalization]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_migrate.c#L426-L471
[svm-placement]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_svm.c#L3501-L3582
[svm-migration-result]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_svm.c#L3608-L3639
[svm-attribute-update]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_svm.c#L3801-L3892
[svm-eviction]: https://github.com/torvalds/linux/blob/fe2ec83746e501645709761605c2464a44fd2929/drivers/gpu/drm/amd/amdkfd/kfd_svm.c#L3641-L3737
[svm-discard-prefetch-owner]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L4120-L4239
[hip-prefetch-batch]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_hmm.cpp#L566-L688
[clr-prefetch-command]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/platform/command.hpp#L2285-L2311
[svm-operation-storage]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L3768-L3790
[svm-api-errors]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L221-L245
[svm-handler-registration]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L965-L988
[svm-handler-retirement]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L2030-L2045
[clr-signal-reset]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L997-L1002
[clr-signal-reuse]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L779-L805
[hip-source-order]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/hip/include/hip/driver_types.h#L469-L493
[hip-batch-declaration]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/hip/include/hip/hip_runtime_api.h#L6344-L6361
[hip-batch-validation]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_memory.cpp#L3245-L3314
[hip-copy-metadata]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_memory.cpp#L2902-L2959
[hip-batch-enqueue]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_memory.cpp#L2961-L3011
[clr-write-command]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/platform/command.hpp#L1324-L1364
[clr-command-enqueue]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/platform/command.cpp#L382-L446
[clr-batch-write]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocblit.cpp#L2918-L3028
[clr-staging-buffer]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocblit.cpp#L1043-L1084
[hip-batch-classification]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/hipamd/src/hip_memory.cpp#L3103-L3186
[clr-batch-copy]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocblit.cpp#L3160-L3280
[clr-batch-copy-join]: https://github.com/ROCm/rocm-systems/blob/105dd4ff35798f95646353bc08f6c885416ae17e/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L3750-L3816
