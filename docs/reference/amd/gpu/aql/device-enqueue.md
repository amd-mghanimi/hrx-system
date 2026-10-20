# Device-enqueued kernels

A GPU kernel can describe child work in memory for a GPU scheduler to turn
into AQL dispatches. ROCm's OpenCL implementation uses two queues: a software
queue holding captured arguments, dependencies and descendant counts, and a
native HSA queue carrying child dispatches and scheduler continuations.
The scheduler runs in passes and stops relaunching when the root's descendants
finish.
This source-selected protocol supplies a concrete device producer and consumer;
its software records are not additional architected AQL packet types.
[Device producer][enqueue] [Device scheduler][scheduler] [Host launch][launch]

## Applicability and compiler handoff

CLR enables `dynamicParallelism()` from the hidden completion-action argument
only when `!amd::IS_HIP`. Its OpenCL blit program includes the scheduler entry.
This path therefore does not establish HIP dynamic-parallelism behavior or
support on every combination of GPU, compiler and runtime.
[Runtime selection][selection] [Blit-program selection][blit-selection]
[Scheduler entry][scheduler-entry]

The public device-queue API checks context/device membership, properties,
requested byte size and the device-queue count. The ROCm capability assignments
advertise 256 KiB preferred size, 8 MiB maximum and one device queue per
context/device. These are runtime limits. `CL_QUEUE_ON_DEVICE_DEFAULT`
registers the default queue; creation of a nondefault queue alone does not.
Explicit `T_QUEUE` arguments and a present hidden default queue are separate
routes to virtual storage and scheduler-queue creation. The default-queue
route also requires the dynamic-parallelism flag.
[API admission][admission] [Advertised capacities][queue-capabilities]
[Default selection][default-selection] [Queue registration][context]
[Explicit queue argument][queue-argument] [Hidden arguments][dispatch]

The compiled child is identified by a runtime handle:

| Member | Representation and producer |
| --- | --- |
| `kernel_object` | Global pointer to the child kernel descriptor, resolved by CLR after loading the executable. |
| `private_segment_size` | Unsigned 32-bit private-segment byte requirement. |
| `group_segment_size` | Unsigned 32-bit fixed group-segment byte requirement; local arguments can extend it. |

Clang creates the child wrapper and its `.runtime.handle` global in
`.amdgpu.kernel.runtime.handle`. LLVM exports the handle and associated child,
then emits `.device_enqueue_symbol` metadata. CLR parses that metadata,
loads/freezes the executable, and fills the symbol through `Kernel::postLoad`.
The device enqueue routine reads the handle's three fields. The similarly
named `AmdVQueueHeader::kernel_table` remains zero in the cited initializer
and is not consumed by this ROCm device-library family.
[Compiler handle][compiler] [Export][export] [Metadata][metadata]
[Metadata name][metadata-name] [Runtime metadata reader][metadata-reader]
[Load order][load] [Handle contents][handle] [Device read][enqueue]

The compiler selects four enqueue entry points, rather than one variadic
packet interface:

| Source argument form | Device entry |
| --- | --- |
| Captured block | `__enqueue_kernel_basic` |
| Block and event wait/result arguments | `__enqueue_kernel_basic_events` |
| Block and local argument sizes | `__enqueue_kernel_varargs` |
| Block, events and local argument sizes | `__enqueue_kernel_events_varargs` |

Clang's block header carries its byte size and alignment. The enqueue routine
copies the captured bytes into a reserved slot before returning; temporary
local-size arrays need not outlive that call. Captured pointers remain pointers:
the pointed-to payload is borrowed through its actual child or descendant
users. The child wrapper receives the capture by value and calls the original
block function. [Block layout][block] [Overload selection][lowering]
[Child wrapper][compiler] [Capture copy][enqueue]

## Software queue representation

CLR allocates one backing object for the header, dispatch wrappers, argument
and wait-list regions, events, and allocation masks. It initializes a zeroed
CPU shadow, assigns each wrapper its permanent kernarg/wait-list addresses,
and uploads the image before use. The following offsets are derived from the
cited 64-bit device-library and host structures. [Device structures][devenq]
[Host structures][host-structures]
[Backing construction][allocation]

| `AmdVQueueHeader` byte offset | Field and meaning |
| --- | --- |
| 0, 4 | `aql_slot_num`, `event_slot_num`: unsigned 32-bit entry counts. |
| 8 | `event_slot_mask`: 64-bit address of event allocation bits. |
| 16 | `event_slots`: 64-bit address of event records. |
| 24 | `aql_slot_mask`: 64-bit address of wrapper allocation bits. |
| 32 | `command_counter`: unsigned 32-bit command-ID allocator. |
| 36 | `wait_size`: unsigned 32-bit maximum wait-list entry count. |
| 40 | `arg_size`: unsigned 32-bit per-wrapper argument extent in bytes. |
| 44 | `mask_groups`: unsigned 32-bit count of 32-entry mask words assigned to each scheduler workitem. |
| 48 | `kernel_table`: 64-bit field left zero by this initializer. |
| 56–63 | `reserved[2]`; total header size is 64 bytes. |

| `AmdAqlWrap` byte offset | Field and meaning |
| --- | --- |
| 0 | `state`: unsigned 32-bit software state. |
| 4 | `enqueue_flags`: unsigned 32-bit parent-readiness selection. |
| 8 | `command_id`: unsigned 32-bit ID from `command_counter`. |
| 12 | `child_counter`: unsigned 32-bit count of outstanding immediate children. |
| 16 | `completion`: 64-bit device event pointer, or zero. |
| 24 | `parent_wrap`: 64-bit pointer to the parent wrapper. |
| 32 | `wait_list`: 64-bit pointer to retained device-event handles. |
| 40 | `wait_num`: unsigned 32-bit wait-list length. |
| 44–63 | `reserved[5]`. |
| 64–127 | `aql`: ordinary 64-byte kernel-dispatch packet, including its initialized kernarg pointer. |

`AmdEvent` is 40 bytes: unsigned 32-bit `state` and reference `counter`, three
64-bit `timer` values, then a 64-bit `capture_info` output pointer. These
private event records are distinct from native HSA signal objects.
[Complete declarations][devenq]

The advertised `arg_size` is `maxParameterSize + 64`. The backing stride also
includes `numWaitEvents * 8` bytes and rounds up to 128 bytes. The requested
queue byte size describes wrapper capacity, not the full backing allocation
and not a native ring's packet count. Separately, CLR requests a 2048-entry
`HSA_QUEUE_TYPE_MULTI` queue with private/group requests `UINT_MAX`.
[Allocation arithmetic][allocation] [Native queue creation][queue]

## Capture publication and wrapper states

`reserve_slot` searches mask words with relaxed device-scope compare/exchange.
A set bit claims the resource; a failed full pass returns `-1`. It does not
publish a ready dispatch or store the declared RESERVED state. Accepted
enqueue fills capture/implicit arguments and the wrapper, increments the
parent's child counter, then stores READY with release/device ordering.
The scheduler loads wrapper state with acquire/device ordering.
[Reservation and release][devenq] [Producer][enqueue] [Consumer][scheduler]

| State | Value | Selected role |
| --- | ---: | --- |
| `AQL_WRAP_FREE` | 0 | Initialized or released wrapper; the mask separately owns allocation. |
| `AQL_WRAP_RESERVED` | 1 | Declared state, excluded by the marker's minimum-command scan; `reserve_slot` itself claims only the mask. |
| `AQL_WRAP_READY` | 2 | Published child description, waiting for scheduling conditions. |
| `AQL_WRAP_MARKER` | 3 | Event/command-order marker without a kernel dispatch. |
| `AQL_WRAP_BUSY` | 4 | Child selected in a scheduler pass. |
| `AQL_WRAP_DONE` | 5 | Child execution has joined; descendants can still retain the wrapper. |

The actual enqueue guards bound captured-plus-implicit arguments, the `mul24`
workgroup-size product against 256, applicable wait-list counts and a 64 KiB
local-argument allocation. Local offsets align to 16 bytes. Those source
checks and compiler-produced inputs are not a complete parser contract for
arbitrary record bytes. [All four producers][enqueue]

Implicit arguments depend on `__oclc_ABI_version`: the source reserves seven
`size_t` slots below 500 and 32 slots otherwise. The modern basic,
basic-events and varargs writers initialize slots 24/25 for aperture bases
and the HSA queue pointer. The modern events-plus-varargs writer stops after
slot 14, while accessors exist for 24/25 and LLVM emits their metadata when
the target/usage requires them. The four writers therefore cannot be assumed
equivalent for a child that consumes those fields. This is a source-level
producer difference, not an observed execution failure.
[Argument writers][enqueue] [Implicit readers][devenq]
[Conditional metadata][metadata]

## Scheduler passes and native AQL

CLR creates a root wrapper with zero child count and state DONE before
launching the parent. DONE here is initialized software state, not early
evidence of parent execution completion: the host queue places a
SYSTEM-fenced barrier after the parent, then dispatches the first scheduler.
The scheduler uses one workitem per workgroup. Its arguments include the
virtual/native queue addresses, root wrapper, a separate completion signal
initialized to one, and a saved packet for self-relaunch.
[Parent and launch sequence][dispatch] [Host barrier header][barrier-header]
[Scheduler arguments][launch]

Each scheduler workitem scans its assigned `mask_groups * 32` wrappers and
launches at most one READY child in a pass. Both WAIT_KERNEL and
WAIT_WORK_GROUP require parent state DONE in this implementation; they do
not implement distinct workgroup-level joins. Event dependencies can defer a
READY child to another pass. This is pass-based scheduling after parent/child
execution boundaries, not a concurrently resident scheduler serving a parent
that remains blocked waiting for its own child.
[Selection loop][scheduler]

The device-built child packet uses these fields:

| Field | Selected value |
| --- | --- |
| Header | KERNEL_DISPATCH, barrier bit 0, AGENT acquire and release; numeric header `0x0a02`. |
| Setup | Work dimension from the supplied `ndrange_t`. |
| Workgroup dimensions | Three local dimensions narrowed to unsigned 16-bit fields. |
| Grid dimensions | Three global workitem counts narrowed to unsigned 32-bit fields. |
| Kernel object and private bytes | From the per-child runtime handle. |
| Group bytes | Fixed handle requirement plus the selected local-argument layout. |
| Kernarg address | The wrapper's initialized argument slot. |
| Completion signal | Zero; software wrapper/event tracking supplies the child/descendant protocol. |

[Packet producer][enqueue] [AQL header definitions][hsa-header]
[Dispatch and compiler ABI](dispatch.md)

`EnqueueDispatch` reserves a monotonic native queue index and copies the
packet to `index & (size - 1)`. With PCIe atomics, reservation uses OCKL's
all-SVM queue-index atomic; otherwise it uses the scheduler parameter's
device-local index. In the latter case, scheduler relaunch stores the native
write index before ringing the doorbell. OCKL uses release ordering for the
doorbell and has a separate older-ISA lock/monotonic notification path.
Windows also has a host notifier watching scheduler-queue progress.
[Queue producer][scheduler] [OCKL operations][ockl]
[Windows notifier][notifier]

After every workitem increments a relaxed device-scope pass counter, the last
workitem either completes the tree or appends the saved scheduler packet.
That packet has barrier=1 and SYSTEM acquire/release. CLR constructs ordinary
and selected EXT_KERNEL_DISPATCH versions explicitly. The relaunch waits for
preceding children, and its next pass changes their BUSY states to DONE.
Only a DONE wrapper with zero outstanding children drops its parent's count,
returns FREE and releases its allocation bit. The retained wrapper/kernarg
therefore spans both child execution and descendant completion.
[Saved packet][dispatch] [Pass and reclamation][scheduler]

The source assigns whole packet structures containing valid headers; its
pass counter is relaxed. Those statements plus a later doorbell release do
not by themselves specify the compiler-store and CP-observation contract
needed for portable body-before-header publication. The ordinary
[publication protocol](publication.md#packet-publication)
remains a separate requirement. This scheduler implementation is evidence of
an actual producer, not authority for replacing that protocol with any
whole-structure store. [Device stores][scheduler] [Doorbell store][ockl]

### Capacity arithmetic

For an admitted request `r` from zero through 8 MiB, CLR computes:

```text
s = max(r, 16 KiB)
m = max(1, floor(s / (512 KiB)))
q = 128 * 32 * m
scheduler_workitems = ceil(s / q)
```

The maximum is 256 workitems; for example, `r=1,048,575` rounds to
1,048,576 bytes with `m=1`. Preferred 256 KiB gives 64 workitems. The selected
loop consequently emits at most 256 children and one relaunch per pass,
below its requested 2048 native entries. This is host arithmetic and a
per-pass bound, not a complete ring-occupancy proof. The device enqueue leaves
contain no read-index capacity wait. [Admitted range][admission]
[Maximum queue bytes][queue-capabilities]
[Rounding][allocation] [Emission loop][scheduler]

## Events, clocks and final users

An event returned by kernel enqueue starts with two references, for scheduler
and caller. A user-created event starts with one. Copied wait lists retain
each event; scheduling or marker completion consumes those references.
Completion releases the scheduler reference, and the last reference release
clears the event allocation bit. A marker without a wait list compares its
command ID with the minimum among non-FREE/non-RESERVED wrappers; it is not
an AQL barrier packet. [Event functions][events] [Event producer][enqueue]
[Scheduler event handling][scheduler]

The scheduler recognizes negative waited-event status by converting its
unsigned state to signed. Its own `event->state >= 0` guards instead compare
an unsigned field and are always true. Those guards therefore do not preserve
an earlier stored `-1` as a signed comparison would. The normal-success flow
above does not establish error propagation through this source discrepancy.
[State type][devenq] [Status comparisons][scheduler]

START is sampled when scheduling the child, END when a later pass joins its
execution, and COMPLETE after descendants finish. These scheduler-observed
intervals include scheduling gaps. For a raw `__builtin_readcyclecounter()`
sample `C`, the source stores `(C * eng_clk) >> 10`, using unsigned 64-bit
arithmetic. CLR sets `eng_clk` to integer `(1000 * 1024) / F`, where `F` is
`HSA_AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY` in MHz. A zero reported maximum
leaves the zero-initialized factor. This is a reciprocal of the reported
maximum, not a measurement of the running shader clock. [Sampling][scheduler]
[Scale construction][launch] [Frequency query][clock-query]
[Property units][clock-units]

The compiler's [shader-cycle selection](../observability.md#shader-cycle-counters)
includes zero-extended 20-bit values and 64-bit shader counters. RDNA3/3.5,
RDNA4 and CDNA5 define those shader counters for deltas within one wave,
with unsynchronized SIMD epochs. START and END here belong to separate
scheduler dispatches. Both duration writers subtract the already scaled
timestamps; neither reconstructs narrow-counter rollover or reconciles
epochs. Consequently the cited implementation does not establish portable
cross-pass elapsed time on those clock contracts. Selecting a fixed-frequency
counter would also require its own frequency conversion. [Actual
subtractions][scheduler] [Immediate capture][events]
[Clock domains](../observability.md#shader-realtime-and-frequency)
[Packet-processor profiling](profiling.md)

`capture_event_profiling_info` stores an output pointer in the event. If its
acquire load already observes CL_COMPLETE, the calling workitem writes both
durations immediately. The scheduler can also write the durations; it stores
CL_COMPLETE before its optional output stores. Event status alone therefore
does not retire every output writer. Output storage remains borrowed through
the capture call and any scheduler writer, while the event record has its own
reference lifetime. [Immediate capture writer][events]
[Scheduler store order][scheduler]

When the root count reaches zero, the scheduler performs a relaxed OCKL store
of zero to its separate completion signal. CLR adds that signal to the host
queue's dependency list and submits a SYSTEM-fenced AND barrier with its own
tracker completion. The no-PCIe-atomics route also waits on the CPU before
returning. The shader-written zero occurs before the scheduler's dispatch
completion and OCKL's notification tail; observing it alone does not retire
those users or establish that their storage can be destroyed.
[Terminal producer][scheduler] [Signal notification][ockl]
[Host dependency][wait] [Barrier scopes][barrier-header]
[Conditional CPU wait][launch]

Host ownership supplies further boundaries: managed kernarg chunks have
barrier signals before recycling; captured OpenCL arguments retain memory,
sampler and explicit queue objects; the NDRange command retains its kernel
and program. Normal flush waits its fence before terminal command status
releases captured resources. Arbitrary captured SVM/global pointers still
require application lifetime ownership.
[Argument ownership][arguments] [Host and pool retirement][retirement]
[Command capture owner][command-owner] [Kernel's program reference][program-owner]
[Reference lifetime][shared-reference] [Flush ordering][flush]
[Terminal command update][command-update] [Terminal resource release][event-status]

Teardown follows tracked host completion when present, stops the Windows
notifier, destroys the native scheduler queue, then releases virtual backing.
ROCr inactivates the native queue before freeing its ring. Native removal and
successful payload execution remain distinct: HSA queue destruction does
not guarantee unfinished work produced its results. The cited error paths
also matter: CLR discards `runScheduler`/queue-destroy results, and ROCr's
inactivation checks native destruction through an assertion. These calls do
not establish successful failed-wait/removal recovery or a general concurrent
virtual-pool replacement protocol. [CLR teardown][retirement]
[Dynamic caller][dispatch] [ROCr teardown][native-destroy]
[Native inactivation][native-inactivate]
[HSA destruction contract][hsa-destroy]

[compiler]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/clang/lib/CodeGen/Targets/AMDGPU.cpp#L610-L735
[block]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/clang/lib/CodeGen/CGBlocks.cpp#L480-L1121
[lowering]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/clang/lib/CodeGen/CGBuiltin.cpp#L6442-L6628
[export]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPUExportKernelRuntimeHandles.cpp#L60-L110
[metadata]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/llvm/lib/Target/AMDGPU/AMDGPUHSAMetadataStreamer.cpp#L255-L735
[load]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocprogram.cpp#L200-L297
[handle]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rockernel.cpp#L15-L164
[enqueue]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/opencl/src/devenq/enqueue.cl#L7-L541
[devenq]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/opencl/src/devenq/devenq.h#L1-L189
[events]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/opencl/src/devenq/events.cl#L1-L73
[scheduler]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/opencl/src/devenq/schedule_rocm.cl#L5-L239
[ockl]: https://github.com/ROCm/llvm-project/blob/8cd9ac8c8f12ab07e92229ea9d690b49e866b8ec/amd/device-libs/ockl/src/hsaqs.cl#L27-L185
[admission]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/opencl/amdocl/cl_command.cpp#L59-L188
[context]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/platform/context.cpp#L394-L422
[selection]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/devkernel.cpp#L882-L1029
[allocation]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L4587-L4698
[queue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L4489-L4527
[dispatch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L4708-L5153
[launch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocblit.cpp#L3754-L3819
[notifier]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L4529-L4578
[wait]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2160-L2231
[arguments]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/platform/kernel.cpp#L212-L354
[retirement]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2347-L2695
[native-destroy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L354-L417
[hsa-header]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2864-L2950
[hsa-destroy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa.h#L2526-L2571
[blit-selection]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocdevice.cpp#L792-L816
[scheduler-entry]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocblitcl.cpp#L11-L15
[queue-capabilities]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocdevice.cpp#L1708-L1713
[default-selection]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/platform/commandqueue.cpp#L381-L398
[queue-argument]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1193-L1202
[metadata-name]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/platform/kernel_init.hpp#L86-L105
[metadata-reader]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/devkernel.cpp#L447-L591
[host-structures]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocsched.hpp#L11-L77
[command-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/platform/command.cpp#L451-L490
[program-owner]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/platform/kernel.hpp#L280-L306
[shared-reference]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/platform/object.hpp#L168-L182
[flush]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L5414-L5419
[command-update]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2924-L3000
[event-status]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/platform/command.cpp#L110-L178
[native-inactivate]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L737-L746
[barrier-header]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L71-L74
[clock-query]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocdevice.cpp#L1186-L1196
[clock-units]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L732-L736
