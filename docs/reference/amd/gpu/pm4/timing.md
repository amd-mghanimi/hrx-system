# PM4 timing

A timestamp records a clock at a selected execution stage. Its interpretation
requires the clock domain and units; its observation requires a completed store
and a visibility protocol. Programmable counters add block configuration,
instance selection and device ownership. These are separate mechanisms even
when both eventually copy numeric values into memory.

## Timestamp stage and representation

PAL's GFX10/GFX11 compute timestamp caller selects COPY_DATA for CP stages
and RELEASE_MEM when the requested stage includes compute shaders or
bottom-of-pipe. It handles outstanding CP DMA separately. Mesa likewise uses
COPY_DATA at top-of-pipe and an EOP timestamp event for later stages. A CP
sample placed after a dispatch packet need not follow completion of that
asynchronous shader.
[PAL stage selection][pal-stage] [Mesa stage selection][mesa-stage]

| Mechanism | Native fields and meaning |
| --- | --- |
| CP clock sample | Six-DWORD COPY_DATA: GPU-clock source 9, destination 5, 64-bit count and write confirmation. Source words are zero; destination is eight-byte aligned. The ordinary control word is `0x00110509`. |
| Shader/end-of-pipe sample | Eight-DWORD RELEASE_MEM with BOTTOM_OF_PIPE_TS, EOP index 5, GPU-clock data selector 3, TC/L2 destination and confirmation without interrupt. Cache actions and CP-DMA wait remain separate choices. |
| Other clock selector | COPY_DATA source 10 is named system_clock_count; RELEASE_MEM also has a distinct system-clock selector. A shared name does not establish a common epoch with CPU clocks or other engines. |

[Copy selectors and layout][copy-fields] [Release selectors and layout][release-fields]
[Release construction][release-builder]

PAL names COPY_DATA destination 5 `tc_l2_obsolete`; Mesa calls its ordinary
timestamp destination memory. Their timestamp callers use that numeric route,
whereas the ordinary TC/L2 copy destination is 2. The source convention should
be preserved rather than changed from a name alone.
[PAL timestamp caller][pal-stage] [Mesa timestamp caller][mesa-stage]

Equal successive samples can be valid when observation intervals are shorter
than the counter resolution. An ordinary nondecreasing comparison presumes no
wrap or reset in the interval. Modular subtraction needs the valid counter
width and an interval bound excluding multiple wraps. Neither a 64-bit transfer
nor a plausible delta establishes an indivisible live read or reset continuity.

## Timestamp visibility and storage lifetime

A CP-begin to shader-end bracket uses separate aligned 64-bit result slots,
valid executable/argument storage and the native queue's publication protocol.
Input producers complete their release before the consumer acquires those
inputs. Any preceding work excluded from the interval must first be joined
through its own execution dependency. A top-of-pipe begin sample does not
itself drain earlier shaders. Binding the program before the begin sample
excludes those binding commands from this particular interval; launch and
scheduling remain inside it. [Stage selection][pal-stage]
[Shader dependencies](dispatch.md#publication-and-shader-dependencies)

PAL's timing-only `GpaSession` allocates the begin/end slots together and
records the configured pre-sample in `BeginSample`. For a top-of-pipe begin
and compute-shader end, the following sequence traces its GFX11 compute path:

```text
publish executable, arguments, inputs and command storage
  → join input producers and any excluded preceding work
  → acquire inputs and bind the program
  → confirmed COPY_DATA GPU-clock sample into begin
  → DISPATCH_DIRECT
  → RELEASE_MEM GPU-clock sample into end at shader/end-of-pipe completion
  → BOTTOM→TOP CoherCp→CoherMemory barrier
  → bottom-of-pipe session event
  → repeat the CoherCp→CoherMemory barrier
  → host readiness check → read the mapped begin/end slots
  → retire the complete submission before reusing its remaining storage
```

`EndSample` records the end timestamp. `End` flushes sampled data, sets the
session event and then flushes event data. `IsReady` observes that event;
additional queue-fence polling is conditional on the separate queue-timing
feature. `TimingSample` reads both mapped uint64 values directly.
[Begin sample and allocation][sample-begin] [End sample][sample-end] [Session end][session-end]
[Readiness][session-ready] [Result storage][sample-read]

For the GFX11 non-PWS compute path, the CP-to-memory access transition requests
GL2 writeback. Barrier lowering uses `WriteWaitEop`: write a known value to
private fence storage, wait for that exact value, then perform cache work not
represented in the release. The equality wait is the execution join; the
following ACE ACQUIRE_MEM immediately performs cache operations.
[Cache mapping][cpu-cache] [Barrier lowering][barrier] [EOP join][wait-eop]

PAL allocates that private fence from the command allocator and emits a zero
write on first use. Later waits advance its value. Its compute WAIT_REG_MEM
uses full-mask equality, polling interval 10 and ACE offload. The fence remains
owned through the wait; a later event at a different address is not a substitute
for this join/writeback sequence.
[Fence allocation][fence-owner] [Value progression][fence-value]
[Wait construction][wait-fields]

MEC full-range ACQUIRE_MEM uses low size `0xffffffff`, high size `0xff` and
zero base with reserved bits clear. Its GCR mask is selected for the actors;
GL2 writeback does not also mean instruction or scalar-cache invalidation.
The wider GFX11 graphics range field is a different form.
[MEC cache operation][mec-acquire]

Readiness and all-storage retirement remain distinct. PAL records cache/fence
work after its session-event store. A caller reclaiming command buffers,
private fences or timestamp storage must account for that tail through its
submission owner; observing the event alone is not evidence that later
commands have retired. [Session tail][session-end]
[Command-storage lifetime](command-buffers.md)

A CP-begin to shader-end interval includes launch, scheduling and stage overhead.
It is not an isolated shader instruction duration. Host end-to-end timing adds
submission and completion observation; submit-only timing deliberately excludes
completion from the interval. Each observation site needs its own stated
boundary.

## GFX12 timestamp confirmation

The [release selectors](release.md#confirmation-and-notification-selectors)
separate producing a clock sample from confirming its write or interrupting an
observer. The following path deliberately defers confirmation.

PAL's GFX12 compute timestamp caller retains stage selection and the CP-DMA
join, but defers write confirmation. CP-stage `COPY_DATA` uses GPU-clock
source 9, destination 2 (`tc_l2`), 64-bit count and `WR_CONFIRM=0`.
Its word-1 control value is `0x00010209`; the selected source/destination
temporal hints are zero. CS/bottom-stage `RELEASE_MEM` samples use
`BOTTOM_OF_PIPE_TS` and GPU-clock data with `noConfirmWr=true`, leaving
`INT_SEL=0`. The universal command-buffer caller makes the same confirmation
choice. These are different selections from the older confirmed samples above.
[Compute timestamp caller][pal12-timestamp-caller]
[Universal timestamp caller][pal12-timestamp-universal]
[COPY_DATA fields][pal12-timestamp-copy-fields]
[COPY_DATA builder][pal12-timestamp-copy]
[Release confirmation][pal12-timestamp-release]

The public timestamp API requires the matching source-stage flags and
`CoherTimestamp` access. GFX12 barrier planning keeps that access as a separate
timestamp-completion requirement, alongside the cache mask. Its confirmation
helper emits a confirmed EOP GPU-clock write to private scratch, followed by
a confirmed one-DWORD `WRITE_DATA` to the same scratch address. PAL attributes
these to confirmation of the EOP and CP write paths. The scratch value is not
the user's timestamp or a readiness flag: the command allocator provides an
eight-byte, eight-byte-aligned slot whose content is otherwise ignored.
[Timestamp API][pal12-timestamp-api] [Separate requirement][pal12-timestamp-requirement]
[Confirmation sequence][pal12-timestamp-sync]
[WRITE_DATA confirmation][pal12-timestamp-write]
[Scratch owner][pal12-timestamp-scratch]

For a compute-stage timestamp consumed by a shader, an ordinary combined
buffer barrier declares `CS/CoherTimestamp → CS/CoherShaderRead`. The
bypass-to-shader transition requests GL2 writeback/invalidation and K$/V$
invalidation. The timestamp requirement promotes the CS join to EOP. On the
compute engine the resulting source sequence is:

```text
unconfirmed timestamp store into the result slot
  → confirmed EOP clock store and confirmed DWORD store into private scratch
  → EOP release carrying data-cache actions and a private fence value
  → equality wait on that fence → consuming shader
```

The timestamp result, helper scratch and completion fence are distinct storage
roles. A CPU/memory destination can omit the GL2 actions under PAL's bypass
classification while retaining confirmation and the execution join. Later
host observation and storage reuse still require the mapping, completion and
retirement contract; the unconfirmed sample alone supplies none of those.
[Access and cache planning][pal12-timestamp-requirement]
[Event selection][pal12-timestamp-events]
[Confirmation before dependency][pal12-timestamp-combined]
[Compute fence and wait][pal12-timestamp-eop]

`CoherCp` is a distinct access bit. The shared `GpaSession::End` caller uses
it in the session flow above and therefore does not directly request this
dedicated timestamp helper; its GFX12 compute lowering still includes an EOP
fence and wait. That source path does not replace the timestamp API's explicit
`CoherTimestamp` contract or establish that packet retirement makes an
unconfirmed sample ready. [Access definitions][pal12-timestamp-access]
[Session caller][session-end] [Compute EOP join][pal12-timestamp-eop]

## Clock units and host correlation

| Surface | Contract and implication |
| --- | --- |
| Device frequency | PAL exposes timestamp frequency in Hz; its AMDGPU backend multiplies `gpu_counter_freq` in kHz by 1000. Linux derives that field from ASIC xclk. RADV uses `1,000,000 / frequency_kHz` nanoseconds per tick. Dynamic shader core MHz is not this counter frequency. |
| Valid width | RADV exposes 64 valid timestamp bits for its graphics and compute families. Transfer width, increment period, read quantization and observation overhead remain separate quantities. |
| KFD clock snapshot | The driver reads the GPU counter, raw monotonic host time and boottime sequentially. `system_clock_freq=1,000,000,000` describes host nanosecond units, not the GPU frequency or simultaneous sampling. |
| Native GFX11 reader | Linux reads CP MES time for an SR-IOV VF and Golden TSC otherwise, with an SMUIO-version branch and rollover-consistent high/low reads. Virtualization and native IP therefore affect the observation path. |
| Host calibration | PAL brackets `AMDGPU_INFO_TIMESTAMP` with monotonic and monotonic-raw CPU reads and reports the larger bracket as maximum deviation. This exposes sampling uncertainty instead of treating the query as simultaneous. |

[Frequency property][pal-frequency] [Unit conversion][frequency-conversion]
[Linux frequency][linux-frequency] [RADV period][mesa-period]
[RADV valid width][mesa-width] [KFD sampling][kfd-clock]
[KFD clock ABI][kfd-clock-abi] [GFX11 counter reader][linux-clock]
[Calibration][calibration]

A compiler target string does not identify clock continuity across reset,
suspend, virtual machines, devices or engines. Source 9, source 10, shader clock
instructions, SDMA timestamps and AQL signal timestamps cannot be correlated
solely from their labels. A usable correlation names the native domain,
frequency, valid width, sampling deviation and reset epoch.

## Programmable counter ownership

The [counter chapter](counters.md) follows a concrete GFX11 collection from
event and instance selection through register programming, result visibility
and completed submission retirement. It distinguishes raw event counts from
the clock samples described here.

PAL's `PerfExperiment::IssueBegin` opens a profiling window, waits for prior
work, disables/resets global and streaming counters and chooses always-count
mode. The source explicitly says that path does not support per-context
filtering. Block, instance, selector and filtering state determine what is
counted; a queue does not thereby exclude unrelated GPU activity.
[Experiment start][counter-begin]

Sampling has its own ordered protocol. PAL samples and waits idle before
controlling global counters; its `neverStopCounters` policy alters the stop
behavior. The later 64-bit read copies low/high registers separately. Those two
copies are not intrinsically an atomic live-counter snapshot: stability comes
from the block's stop/latch and instance-selection protocol.
[Sampling][counter-sample] [Two-part read][counter-read]

`IssueEnd` stops/samples, resets/disables global state, restores SQG event
configuration and closes the window. Selection registers are intentionally left
because global enables are off. This is terminal cleanup for an owned
experiment, not preservation of arbitrary preexisting counter state belonging
to another owner. [Experiment cleanup][counter-end]

On GFX11, PAL's window builder depends on MEC image version at least 2290 or
PFP image version at least 2240 for the respective engine. Unsupported window
emission becomes a NOP in that implementation. Perfmon clock control is a
separate privileged KMD service on GFX11; window support does not supply it.
[Window construction][window-builder] [Version constants][window-constants]
[Selection][window-predicates] [GFX11 caller][window-caller]
[Privileged clock boundary][counter-begin]

The cited Linux KFD profiler interface supplies a device/process owner rather
than a per-queue lease. PMC control requires `perfmon_capable()`, and another
process's owner causes busy. Its queue-perfcount update walks the device's
process/queue lists. Process cleanup disables perfcount and clears the matching
owner. These native ownership operations are distinct from emitting counter
packets and from PAL's register cleanup; neither promises arbitrary state
restoration for concurrent experiments.
[Profiler request layout][profiler-abi] [Permission and owner][profiler-owner]
[Queue updates][queue-update] [Process cleanup][process-cleanup]

The exact deployed driver interface, hardware support and owner lifecycle must
match the profiling client. Linux's owner path does not establish the same
protocol for another operating system or firmware transport. Broader clock and
profiling-domain distinctions belong to [observability](../observability.md).

[pal-stage]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L396-L453
[mesa-stage]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_query.c#L2751-L2766
[copy-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L730-L916
[release-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1888-L2074
[release-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3335-L3537
[sample-begin]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L1819-L1856
[sample-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L1982-L2005
[session-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L1537-L1618
[session-ready]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSession.cpp#L2030-L2059
[sample-read]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/gpuUtil/gpaSessionPerfSample.cpp#L805-L853
[cpu-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L353-L365
[barrier]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L1998-L2042
[wait-eop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1778-L1847
[fence-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L445-L462
[fence-value]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.h#L526-L529
[wait-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4488-L4519
[mec-acquire]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L398-L427
[pal-frequency]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palDevice.h#L1029-L1037
[frequency-conversion]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuDevice.cpp#L961-L973
[linux-frequency]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L944-L955
[mesa-period]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L1906-L1918
[mesa-width]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L3002-L3026
[kfd-clock]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L701-L725
[kfd-clock-abi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L181-L190
[linux-clock]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L5301-L5343
[calibration]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuDevice.cpp#L1958-L1994
[counter-begin]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2020-L2060
[counter-sample]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3689-L3744
[counter-read]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L3949-L3965
[counter-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9PerfExperiment.cpp#L2225-L2310
[window-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4871-L4915
[window-constants]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L224-L226
[window-predicates]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L375-L401
[window-caller]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdStream.cpp#L1673-L1684
[profiler-abi]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/include/uapi/linux/kfd_ioctl.h#L1560-L1588
[profiler-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_chardev.c#L3333-L3415
[queue-update]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device_queue_manager.c#L331-L352
[process-cleanup]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_process.c#L1177-L1185
[pal12-timestamp-caller]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L197-L256
[pal12-timestamp-universal]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12UniversalCmdBuffer.cpp#L5853-L5913
[pal12-timestamp-copy-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L665-L766
[pal12-timestamp-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2280-L2312
[pal12-timestamp-release]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1852-L1887
[pal12-timestamp-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L4025-L4048
[pal12-timestamp-requirement]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L375-L485
[pal12-timestamp-sync]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L92-L119
[pal12-timestamp-write]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2109-L2138
[pal12-timestamp-scratch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L464-L477
[pal12-timestamp-events]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L166-L204
[pal12-timestamp-combined]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L1199-L1204
[pal12-timestamp-eop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1721-L1784
[pal12-timestamp-access]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L319-L355
