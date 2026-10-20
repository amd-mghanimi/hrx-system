# Resident NPU file channels

This Linux CTS recipe connects a running NPU program to a CPU-serviced file
channel. One native submission starts the NPU; its next request depends on
the bytes returned by the preceding file read. A single CPU thread performs
`pread`/`pwrite` and the explicit cache maintenance required by the CPU/NPU
memory contract. There is no per-request NPU dispatch, GPU relay, `io_uring`
poller, kernel modification, or assumption of coherent shared memory.

The recipe establishes a user-mode transport and measures its cost. It does
not implement a public Loom channel API, a model workload, or controller-level
NVMe offload. In particular, the CPU service busy-polls for NPU requests; moving
request generation to the NPU does not eliminate that CPU cost.

## Data and ownership

The [NPU program](../../programs/resident_file_channel.loom) emits an initial
batch `Q1[i] = seed + 257 + 17*i`. Each record's first word selects one of 64
immutable input records. The CPU reads that record into the return payload
and writes the NPU-produced record into a separate output bank. On consuming
the actual returned bytes, the NPU produces
`Q(g+1)[i] = Rg[i] + 257*(g+1) + 17*i`, modulo 2^32. The final full batch makes
every word of the last file response observable before retirement.

Each direction has one credit covering a complete batch. Ready and payload
occupy separate cache lines, and every live line has exactly one physical
writer. The CPU invalidates the NPU's ready line before reading it, then
invalidates the complete published payload. After the file operations, it
flushes the returned payload before publishing and flushing its ready word.
The NPU uses fresh shim DMA reads and chained payload/ready writes. Returning
credit proves that all reads of the previous payload have finished.

An ordinary positive generation publishes a complete return. A negative errno
publishes an I/O failure without a payload, causing a terminal device return.
Success requires a final CPU acknowledgement after consuming the closing
batch. Zero rounds and prestart abort exercise paths with no file traffic.
Native completion includes the custom DMA drain; all mappings, registered
backing, commands and context remain owned until that completion.

The tests compare every emitted word with an independent file-data oracle,
check both complete file banks and every allocation guard, verify the terminal
record and native retired point, and check that commands were unchanged.
Actual EOF and read-only-file errors occur after two successful exchanges.
Allocated and registered backing use both process and instance native lifetime
variants. The program is built for NPU4 and NPU5; hardware admission selects
the matching image.

## Comparison

`CompareCpu` alternates the resident-NPU path with a CPU-only path using the
same file, keys, records, arithmetic, transcript copies and batch boundaries.
The CPU path is the software alternative, not a claim about the cost of an
application that must consume the results on the NPU. Both paths perform one
read and one write per record. The shapes are 1, 8 or 64 records of 64 bytes,
and one record of 4096 bytes. Responses complete at batch granularity; this
recipe does not measure independent out-of-order completion.

Ordinary CTS uses small fixed counts. Setting `AMDF_NPU_FILE_REPETITIONS=7`
under a benchmark broker lease enables two warm-up phases followed by seven
alternating CPU/NPU phases, each with 1024 exchanges. Build the exact binary
optimized, without sanitizers, before acquiring the lease. Structured
`AMDF_NPU_FILE` records include every raw batch-cycle sample, whole-invocation
wall time, service-thread CPU time, shape and filesystem type.

A batch cycle starts when the CPU observes one request ready and ends when it
observes the next request ready. For the NPU this includes request acquisition,
file operations, return publication, NPU consumption/transform and the next
publication. The whole-invocation measurement additionally includes native
submission, startup and final completion. Batch latency is not divided by
record count and presented as individual request latency. CPU time is not a
measurement of energy or all kernel/firmware activity.

Files are private, immediately unlinked, bounded to two banks and initialized
outside timing. The workload is warm buffered I/O: a tmpfs result measures the
channel and syscall floor, not NVMe throughput or durable writes. The file
filesystem follows `TEST_TMPDIR`, defaulting to `/tmp`. Machine load, NPU power
policy and hardware-worker exclusion belong in each measurement's evidence.
Alternation limits drift; it does not create isolation from other jobs.
