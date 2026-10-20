# SDMA constant fill

`CONST_FILL` / `CONSTANT_FILL` writes a repeating pattern without reading a
source allocation. Its ordinary DWORD form carries the pattern in a
five-DWORD packet and fills a nonempty, four-byte-aligned destination range.
Earlier DMA engines have different packet and count representations; cache
and compression fields also change independently of that common operation.
[PAL caller][pal-caller] [PAL layouts][pal-layout] [Legacy DMA][linux-si]

## Applicability and caller selection

PAL's DMA command buffer implements `CmdFillMemory` by splitting an aligned
range into constant-fill packets. RADV selects SDMA for its transfer queue;
other queue families select compute or CP DMA. Both callers require the
destination address and byte length to be divisible by four. Their loops emit
no fill packet for a zero-length range. [PAL caller][pal-caller]
[RADV selection][mesa-caller] [RADV splitting][mesa-split]

ROCr's public `hsa_amd_memory_fill` has different routing. It accepts a count
of `uint32_t` elements and returns success for zero count after checking the
pointer. `Runtime::FillMemory` selects a GPU or host path from pointer
information. The GPU path calls `GpuAgent::DmaFill` through `BlitDevToDev`,
which is initialized with `CreateBlitKernel`. Thus the public GPU fill uses a
shader, while the separately implemented SDMA fill builder supplies packet
evidence. Successful public-API execution alone does not demonstrate use of
that builder. [API contract][rocr-api] [API validation][rocr-entry]
[Memory routing][rocr-routing] [GPU caller][rocr-fill-caller]
[Blit selection][rocr-blit-selection] [Shader fill][rocr-shader]

## Representation

PAL, Mesa, and ROCr's SDMA builders describe the following DWORD-fill form.
Each starts from zero or fills every emitted word. Optional policy fields
retain the layout-specific meanings described below. [PAL builder][pal-builder]
[GFX12 builder][pal12-builder] [Mesa builder][mesa-builder]
[ROCr builder][rocr-builder]

| DWORD | Selected fields and units |
| --- | --- |
| 0 | Opcode 11 at bits 7:0, subopcode 0 at 15:8, fillsize 2 at 31:30; header `0x8000000b` when optional policy/scope fields are zero. |
| 1–2 | Low/high halves of the real writable GPU destination address. |
| 3 | Full 32-bit repeating pattern. |
| 4 | PAL GFX10/11 and Mesa encode `byte_length - 1`; PAL GFX12 and ROCr encode `byte_length - 4`. Length is positive and divisible by four. In this DWORD mode the low two encoded count bits are ignored. |

Ignoring those low bits makes the two spellings equivalent. The effective
DWORD count is `(encoded_count >> 2) + 1`: a four-byte fill can therefore
encode count 3 or 0. Neither spelling represents zero work. This equivalence
depends on fillsize 2 and does not convert the legacy direct-count forms
below into count-minus-one packets. [GFX10/11 count interpretation][pal-builder]
[GFX12 count interpretation][pal12-builder]

## Count limits and generation differences

| Source | Observed mode and software bound |
| --- | --- |
| PAL GFX10/11 | `IsGfx10` selects the 22-bit count and `2^22 - 4` byte cap; its GFX11 branch selects 30 bits and `2^30 - 4`. [Builder][pal-builder] [Fields][pal-layout] |
| PAL GFX12 | 30-bit count and `2^30 - 4` byte cap. [Builder][pal12-builder] [Fields][pal12-layout] |
| Mesa | Native SDMA 2.4 or later; below 6 uses `2^22 - 4`, SDMA 6+ uses `2^30 - 4`. [Emitter predicate][mesa-builder] |
| ROCr SDMA builder | Shared 22-bit count and `0x3fffe0` byte split limit. Its larger linear-copy overrides do not change this fill constant. [Fill layout and cap][rocr-layout] [Splitting][rocr-submit] |

These are software bounds. For example, a 22-bit count-minus-one field in
DWORD mode can represent `2^22` bytes, while PAL's selected cap is four bytes
smaller. The loop advances by the actual emitted byte length, preserving
alignment across chunks. [PAL splitting][pal-caller] [Mesa splitting][mesa-split]

The count occupies DWORD 4 bits 21:0 or 29:0 according to the selected
layout; its remaining bits are reserved and zero in these builders.
[PAL count layouts][pal-layout] [GFX12 layout][pal12-layout]
[ROCr layout][rocr-layout]

### Linux native emitter families

The following table records the actual native builders and buffer-function
tables. Except for SI, they emit five DWORDs, with address low/high, pattern,
and count following the header. Their header sets fillsize 0. The fillsize-2
count equivalence above does not apply to these builders.

| Linux backend | Count encoding | Advertised per-packet fill maximum |
| --- | --- | --- |
| SI DMA | Direct DWORD count in the header; four-DWORD packet. | `0xffff8` bytes. [Emitter][linux-si] |
| CIK SDMA | Direct byte count in DWORD 4. | `0x1fffff` bytes. [Emitter][linux-cik] |
| SDMA2.4 | Direct byte count in DWORD 4. | `0x1fffff` bytes. [Emitter][linux24] |
| SDMA3.0 | Direct byte count in DWORD 4. | `0x3fffe0` bytes, explicitly reduced for a hardware limit. [Emitter][linux3] |
| SDMA4.0 backend, native IP below 4.4.0 | Bytes minus one. | `2^22` bytes. [Builder and selection][linux4] |
| SDMA4.0 backend, native IP at least 4.4.0 | Bytes minus one. | `2^30` bytes. [Builder and selection][linux4] |
| SDMA4.4.2 backend | Bytes minus one. | `2^30` bytes. [Emitter][linux442] |
| SDMA5.0 backend | Bytes minus one. | `2^22` bytes. [Emitter][linux5] |
| SDMA5.2 backend | Bytes minus one. | `2^30` bytes; its comment contrasts PAL's smaller limit. [Emitter][linux52] |
| SDMA6.0 backend | Bytes minus one. | `2^30` bytes. [Emitter][linux6] |
| SDMA7.0 backend | Bytes minus one; header bit 16 set. | `2^30` bytes. [Emitter][linux7] |
| SDMA7.1 backend | Bytes minus one; opcode-only header. | `2^30` bytes. [Emitter][linux71] |

Backend names are not exact device-IP matches. Linux discovery routes native
4.4.2/4.4.4/4.4.5 to the 4.4.2 backend, 6.0/6.1 variants and 6.4.0 to the
6.0 backend, and 7.0.0/7.0.1 to the 7.0 backend. The cited switch enumerates
the exact supported revisions. [Native selection][linux-discovery]

SI uses opcode `0xd` at header bits 31:28 and a direct DWORD count at 19:0.
Its next words are destination low, pattern, and destination high shifted
left by 16. It is not the later opcode-11 layout. Linux SDMA2.4's
`fill_num_dw = 7` is an allocation budget despite its five emitted words;
the TTM caller budgets with that value and pads the completed IB separately.
[SI fields][linux-si-fields] [SI emitter][linux-si]
[SDMA2.4 emitter][linux24] [IB budgeting][linux-ttm-fill]
[IB padding][linux-ttm-submit]

The inherited Vega10 and Navi10 headers declare only a 22-bit fill count,
even where the 4.4/4.4.2 and 5.2 builders advertise 30-bit ranges. The 6.0 and
7.1 headers explicitly declare 30 bits. A generated header's name and width
therefore do not resolve every native builder's maximum; these sources must
retain their separate revision and mode attributions. [Vega fields][linux-vega-fields]
[Navi fields][linux-navi-fields] [SDMA6 fields][linux6-fields]
[SDMA7.1 fields][linux71-fields]

## Policy fields

All positions below are in DWORD 0 of `SDMA_PKT_CONSTANT_FILL`. Builders
zero unselected fields; presence in a layout does not select an access policy.

| Layout | Policy fields | Values selected by the cited builder |
| --- | --- | --- |
| PAL GFX10/11 | `sw` at 17:16; the `gfx103Plus` view adds `cache_policy` at 26:24 and `cpv` at 28. | `sw = 0`; MALL-capable devices use the write-side policy helper and its CPV predicate. [Layout][pal-layout] [Builder][pal-builder] |
| PAL GFX12 | `nopte_comp` at 16, `sys` at 20, `snp` at 22, `gpa` at 23, `mall_policy` at 27:26. | Compression and destination MALL policy selected as described below; other listed fields zero. [Layout][pal12-layout] [Builder][pal12-builder] |
| ROCr layout with fields annotated `gfx1250` | `mtype` at 17:16, `sys` at 20, `snp` at 22, `gpa` at 23, `scope` at 25:24, `temporal_hint` at 28:26, `npd` at 29. | Scoped template sets `scope = SYS (3)` and `npd = 1`; the other listed fields remain zero. [Layout][rocr-layout] [Scope values][rocr-scope] [Builder][rocr-builder] |

PAL's GFX10/11 policy helper combines a native default L2 write policy with
the selected MALL bypass bit. CPV depends on a nondefault bypass setting and
valid native L2 policy; it is not simply a test for a nonzero packet field.
These policy bits and page-table no-allocation policy jointly influence MALL
allocation. [Policy helpers][pal-policy]

PAL GFX12 passes `MaybeCompressed()` from the destination allocation.
Its builder names `nopte_comp = 0` compression-write bypass and value 1
compression-write disable. `GetMallPolicy(false)` selects the destination
setting when MALL exists, otherwise zero; its values are regular temporal 0,
non-temporal 1, high-priority temporal 2, and last-use 3.
[Allocation predicate][pal-caller] [Compression selection][pal12-builder]
[Destination policy][pal12-policy]

Linux SDMA7.0 independently names header bit 16 `COMPRESS` and sets it to
one. Its inherited header still calls bits 17:16 `sw`; the separate local
definition establishes the builder's interpretation. Linux 7.1 again emits
an opcode-only header while its header names classic `sw`, `cache_policy`
and `cpv` fields. This zero-policy builder does not corroborate the different
scope/temporal-hint field names in ROCr's annotated layout.
[SDMA7 override][linux7-policy] [SDMA7 builder][linux7]
[SDMA7.1 fields][linux71-fields] [SDMA7.1 builder][linux71]

ROCr's template selection follows its ISA/transport predicate in the
[cache chapter](cache.md#ordinary-user-queue-composition), not an SDMA-IP
comparison. In particular, that predicate and the layout's `gfx1250` comments
are different evidence. A scope or residency hint does not complete a prior
transfer or establish a dependent consumer's cache acquire.

## Execution, completion and lifetime

A complete sequence establishes writable destination mappings, performs the
transport's dependency and cache acquire operations, emits the fill chunks,
then supplies the required ordering and release before publishing completion:

```text
establish destination mapping and wait for its previous users
  → publish the selected fill packet(s) and their surrounding commands
  → drain transfers before an in-stream consumer reads the filled range
  → release payload visibility and signal completion for an external consumer
  → consumer waits, acquires the payload, and uses the result
  → reclaim each allocation after its final consumer
```

PAL and RADV's dependency barriers supply the pending-transfer drain described
in [ordering](ordering.md). The selected transport separately owns
[cache maintenance](cache.md) and [completion stores](fence.md). A fill body
contains no completion address. Merely recording the body or advancing a
consumed command frontier does not prove its payload is ready.

Linux's `amdgpu_ttm_clear_buffer` limits each mapped clear job to 256 MiB,
even when the backend advertises a 1 GiB packet limit. The packet splitter
uses the backend maximum, rounded down to 256 bytes unless a DWORD-aligned
request already fits one packet. Job preparation imports reservation
dependencies and any required VM flush; submission returns a scheduler fence.
Command storage is released against the job's finished/hardware fence.
The mapped destination and its subsequent users still have their own lifetime.
[Outer clear loop][linux-ttm-clear] [Packet sizing][linux-ttm-chunks]
[Preparation][linux-ttm-prepare] [Fill and submission][linux-ttm-fill]
[Command backing][linux-job-storage]

ROCr's SDMA fill implementation routes its body through `SubmitBlockingCommand`
and the shared copy submission envelope, with selected HDP/USER_GCR work and
FENCE or atomic completion. That implementation is distinct from the public
shader-fill route above. Its locally built packets are copied into the ring;
ring storage, filled allocation, completion signal, and optional notification
trailer consequently have separate final users.
[SDMA fill entry][rocr-submit] [Blocking owner][rocr-blocking]
[Body copy and completion][rocr-body] [Completion lifetime](atomics.md#memory-and-lifetime)

[pal-caller]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1196-L1231
[pal-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L1171-L1227
[mesa-builder]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L70-L90
[rocr-api]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2890-L2913
[rocr-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2605-L2637
[linux442]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_4_2.c#L2299-L2328
[linux6]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0.c#L1838-L1867
[rocr-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L561-L610
[rocr-submit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1933-L1947
[rocr-blocking]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L364-L394
[pal-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L245-L310
[pal12-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L225-L283
[pal12-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L949-L984
[pal-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L386-L450
[pal12-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L346-L385
[mesa-caller]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_buffer.c#L265-L294
[mesa-split]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L233-L246
[rocr-entry]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L416-L430
[rocr-routing]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/runtime.cpp#L745-L788
[rocr-fill-caller]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2263-L2265
[rocr-blit-selection]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L1037-L1055
[rocr-shader]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L811-L861
[rocr-body]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L558-L649
[linux-si]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/si_dma.c#L816-L837
[linux-si-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sid.h#L557-L580
[linux-cik]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/cik_sdma.c#L1326-L1346
[linux24]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v2_4.c#L1215-L1235
[linux3]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v3_0.c#L1596-L1616
[linux4]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v4_0.c#L2599-L2637
[linux5]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c#L2033-L2053
[linux52]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v5_2.c#L2040-L2060
[linux7]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L1797-L1817
[linux71]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1744-L1763
[linux-discovery]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2785-L2842
[linux-vega-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/vega10_sdma_pkt_open.h#L2325-L2381
[linux-navi-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/navi10_sdma_pkt_open.h#L3650-L3706
[linux6-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L4325-L4393
[linux71-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L4325-L4393
[linux7-policy]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_0.c#L56-L60
[linux-ttm-fill]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L2567-L2604
[linux-ttm-submit]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L169-L182
[linux-ttm-prepare]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L2464-L2491
[linux-ttm-chunks]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L2493-L2513
[linux-ttm-clear]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ttm.c#L2617-L2667
[linux-job-storage]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_job.c#L296-L312
[rocr-scope]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L77-L81
