// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// GENERATED FILE: DO NOT EDIT.
// Generator: loom.gen.ops.c_tables.
// Regenerate: python3 loom/py/loom/gen/run.py c_tables --in-place
// clang-format off

#ifndef LOOM_OPS_CHANNEL_OPS_H_
#define LOOM_OPS_CHANNEL_OPS_H_

#include "loom/ops/op_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  LOOM_OP_CHANNEL_BIND = LOOM_OP_KIND(LOOM_DIALECT_CHANNEL, 0),
  LOOM_OP_CHANNEL_RESERVE = LOOM_OP_KIND(LOOM_DIALECT_CHANNEL, 1),
  LOOM_OP_CHANNEL_ACCEPT = LOOM_OP_KIND(LOOM_DIALECT_CHANNEL, 2),
  LOOM_OP_CHANNEL_WAIT = LOOM_OP_KIND(LOOM_DIALECT_CHANNEL, 3),
  LOOM_OP_CHANNEL_ACQUIRE = LOOM_OP_KIND(LOOM_DIALECT_CHANNEL, 4),
  LOOM_OP_CHANNEL_PUBLISH = LOOM_OP_KIND(LOOM_DIALECT_CHANNEL, 5),
  LOOM_OP_CHANNEL_RELEASE = LOOM_OP_KIND(LOOM_DIALECT_CHANNEL, 6),
  LOOM_OP_CHANNEL_FANOUT = LOOM_OP_KIND(LOOM_DIALECT_CHANNEL, 7),
  LOOM_OP_CHANNEL_COPY = LOOM_OP_KIND(LOOM_DIALECT_CHANNEL, 8),
  LOOM_OP_CHANNEL_SELECT = LOOM_OP_KIND(LOOM_DIALECT_CHANNEL, 9),
  LOOM_OP_CHANNEL_COUNT_ = 10,
};

// Permission on an owned consuming access; absence means immutable reading.
typedef enum loom_channel_mode_e {
  LOOM_CHANNEL_MODE_MUTABLE = 1,
  LOOM_CHANNEL_MODE_COUNT_ = 2,
} loom_channel_mode_t;

// Admission and delivery discipline for a bound channel.
typedef enum loom_channel_bind_discipline_e {
  LOOM_CHANNEL_BIND_DISCIPLINE_FIFO = 1,
  LOOM_CHANNEL_BIND_DISCIPLINE_COUNT_ = 2,
} loom_channel_bind_discipline_t;

// LOOM_OP_CHANNEL_BIND: Bind a fresh channel identity to caller-owned record storage. The leading view dimension indexes slots; capacity bounds the admitted records. The caller retains the allocation through all accesses and transfers. Equal backing addresses do not make two bindings the same protocol.
// %channel = channel.bind<fifo> %storage, %capacity : view<2x144xi32>, index -> channel<tile<144xi32>>
LOOM_DEFINE_ISA(loom_channel_bind_isa, LOOM_OP_CHANNEL_BIND)
LOOM_DEFINE_OPERAND(loom_channel_bind_storage, 0)
LOOM_DEFINE_OPERAND(loom_channel_bind_capacity, 1)
LOOM_DEFINE_RESULT(loom_channel_bind_result, 0)
LOOM_DEFINE_ATTR_ENUM_TYPED(loom_channel_bind_discipline, 0, loom_channel_bind_discipline_t)
iree_status_t loom_channel_bind_build(
    loom_builder_t* builder,
    loom_channel_bind_discipline_t discipline,
    loom_may_consume loom_value_id_t storage,
    loom_may_consume loom_value_id_t capacity,
    loom_type_t result_type,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_channel_bind_facts(
    loom_fact_context_t* context,
    const loom_module_t* module, const loom_op_t* op,
    const loom_value_facts_t* operand_facts,
    loom_value_facts_t* result_facts);
iree_status_t loom_channel_bind_type_transfer(
    loom_type_transfer_context_t* context,
    const loom_module_t* module, loom_op_t* op);
iree_status_t loom_channel_bind_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// LOOM_OP_CHANNEL_RESERVE: Wait for writable storage admission and reserve one producer record. The payload view borrows the write obligation and is uninitialized.
// %write, %view = channel.reserve %channel : channel<tile<144xi32>> -> (write<tile<144xi32>>, view<144xi32>)
LOOM_DEFINE_ISA(loom_channel_reserve_isa, LOOM_OP_CHANNEL_RESERVE)
LOOM_DEFINE_OPERAND(loom_channel_reserve_channel, 0)
LOOM_DEFINE_RESULT(loom_channel_reserve_write, 0)
LOOM_DEFINE_RESULT(loom_channel_reserve_view, 1)
iree_status_t loom_channel_reserve_build(
    loom_builder_t* builder,
    loom_may_consume loom_value_id_t channel,
    loom_type_t write_type,
    loom_type_t view_type,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_channel_access_facts(
    loom_fact_context_t* context,
    const loom_module_t* module, const loom_op_t* op,
    const loom_value_facts_t* operand_facts,
    loom_value_facts_t* result_facts);
iree_status_t loom_channel_selection_type_transfer(
    loom_type_transfer_context_t* context,
    const loom_module_t* module, loom_op_t* op);
iree_status_t loom_channel_reserve_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// LOOM_OP_CHANNEL_ACCEPT: Accept ownership of the next record without waiting for its payload. Mutable acceptance requires exclusive initialized storage at readiness.
// %read = channel.accept %channel : channel<tile<144xi32>> -> read<tile<144xi32>>
LOOM_DEFINE_ISA(loom_channel_accept_isa, LOOM_OP_CHANNEL_ACCEPT)
LOOM_DEFINE_OPERAND(loom_channel_accept_channel, 0)
LOOM_DEFINE_RESULT(loom_channel_accept_read, 0)
LOOM_DEFINE_ATTR_ENUM_TYPED(loom_channel_accept_mode, 0, loom_channel_mode_t)
enum loom_channel_accept_build_flag_bits_e {
  LOOM_CHANNEL_ACCEPT_BUILD_FLAG_HAS_MODE = 1u << 0,
};
typedef uint32_t loom_channel_accept_build_flags_t;
iree_status_t loom_channel_accept_build(
    loom_builder_t* builder,
    loom_channel_accept_build_flags_t build_flags,
    loom_optional uint8_t mode,
    loom_may_consume loom_value_id_t channel,
    loom_type_t result_type,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_channel_access_facts(
    loom_fact_context_t* context,
    const loom_module_t* module, const loom_op_t* op,
    const loom_value_facts_t* operand_facts,
    loom_value_facts_t* result_facts);
iree_status_t loom_channel_accept_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// LOOM_OP_CHANNEL_WAIT: Wait for payload readiness and acquire its visibility before returning a borrowed view. The read remains owned. An unused view does not erase the readiness or acquisition effect.
// %view = channel.wait %read : read<tile<144xi32>> -> view<144xi32>
LOOM_DEFINE_ISA(loom_channel_wait_isa, LOOM_OP_CHANNEL_WAIT)
LOOM_DEFINE_OPERAND(loom_channel_wait_read, 0)
LOOM_DEFINE_RESULT(loom_channel_wait_view, 0)
iree_status_t loom_channel_wait_build(
    loom_builder_t* builder,
    loom_may_consume loom_value_id_t read,
    loom_type_t result_type,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_channel_access_facts(
    loom_fact_context_t* context,
    const loom_module_t* module, const loom_op_t* op,
    const loom_value_facts_t* operand_facts,
    loom_value_facts_t* result_facts);
iree_status_t loom_channel_wait_type_transfer(
    loom_type_transfer_context_t* context,
    const loom_module_t* module, loom_op_t* op);
iree_status_t loom_channel_wait_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// LOOM_OP_CHANNEL_ACQUIRE: Accept the next record and wait for its payload readiness. Mutable consumption permits reading and modifying the initialized payload exclusively until ownership is released or transferred.
// %read, %view = channel.acquire<mutable> %channel : channel<tile<144xi32>> -> (read<tile<144xi32>, mutable>, view<144xi32>)
LOOM_DEFINE_ISA(loom_channel_acquire_isa, LOOM_OP_CHANNEL_ACQUIRE)
LOOM_DEFINE_OPERAND(loom_channel_acquire_channel, 0)
LOOM_DEFINE_RESULT(loom_channel_acquire_read, 0)
LOOM_DEFINE_RESULT(loom_channel_acquire_view, 1)
LOOM_DEFINE_ATTR_ENUM_TYPED(loom_channel_acquire_mode, 0, loom_channel_mode_t)
enum loom_channel_acquire_build_flag_bits_e {
  LOOM_CHANNEL_ACQUIRE_BUILD_FLAG_HAS_MODE = 1u << 0,
};
typedef uint32_t loom_channel_acquire_build_flags_t;
iree_status_t loom_channel_acquire_build(
    loom_builder_t* builder,
    loom_channel_acquire_build_flags_t build_flags,
    loom_optional uint8_t mode,
    loom_may_consume loom_value_id_t channel,
    loom_type_t read_type,
    loom_type_t view_type,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_channel_access_facts(
    loom_fact_context_t* context,
    const loom_module_t* module, const loom_op_t* op,
    const loom_value_facts_t* operand_facts,
    loom_value_facts_t* result_facts);
iree_status_t loom_channel_acquire_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// LOOM_OP_CHANNEL_PUBLISH: Consume an initialized producer reservation and publish its payload with the binding's required visibility.
// channel.publish %write : write<tile<144xi32>>
LOOM_DEFINE_ISA(loom_channel_publish_isa, LOOM_OP_CHANNEL_PUBLISH)
LOOM_DEFINE_OPERAND(loom_channel_publish_write, 0)
iree_status_t loom_channel_publish_build(
    loom_builder_t* builder,
    loom_value_id_t write,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_channel_publish_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// LOOM_OP_CHANNEL_RELEASE: Retire one consuming obligation after its payload uses. Reuse also requires retirement of every other reader or transfer obligation.
// channel.release %read : read<tile<144xi32>>
LOOM_DEFINE_ISA(loom_channel_release_isa, LOOM_OP_CHANNEL_RELEASE)
LOOM_DEFINE_OPERAND(loom_channel_release_read, 0)
iree_status_t loom_channel_release_build(
    loom_builder_t* builder,
    loom_value_id_t read,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_channel_release_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// LOOM_OP_CHANNEL_FANOUT: Transfer one immutable read into two or more independently retiring reads of the same record. This implies neither a payload copy nor a runtime reference count. Mutable reads cannot be fanned out.
// %dma, %history = channel.fanout %read : read<tile<144xi32>> -> (read<tile<144xi32>>, read<tile<144xi32>>)
LOOM_DEFINE_ISA(loom_channel_fanout_isa, LOOM_OP_CHANNEL_FANOUT)
LOOM_DEFINE_OPERAND(loom_channel_fanout_read, 0)
LOOM_DEFINE_VARIADIC_RESULTS(loom_channel_fanout_reads, 0)
iree_status_t loom_channel_fanout_build(
    loom_builder_t* builder,
    loom_may_consume loom_value_id_t read,
    const loom_type_t* result_types,
    iree_host_size_t result_count,
    const loom_tied_result_t* tied_results,
    iree_host_size_t tied_result_count,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_channel_access_facts(
    loom_fact_context_t* context,
    const loom_module_t* module, const loom_op_t* op,
    const loom_value_facts_t* operand_facts,
    loom_value_facts_t* result_facts);
iree_status_t loom_channel_fanout_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// LOOM_OP_CHANNEL_COPY: Transfer both accesses to asynchronous transport. Retire the source after its last source read, independently of publishing the destination after completed writes and required visibility. The enclosing execution owns the transfer even after the issuing helper returns.
// channel.copy %read -> %write : read<tile<144xi32>>, write<tile<144xi32>>
LOOM_DEFINE_ISA(loom_channel_copy_isa, LOOM_OP_CHANNEL_COPY)
LOOM_DEFINE_OPERAND(loom_channel_copy_source, 0)
LOOM_DEFINE_OPERAND(loom_channel_copy_destination, 1)
iree_status_t loom_channel_copy_build(
    loom_builder_t* builder,
    loom_value_id_t source,
    loom_value_id_t destination,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_channel_copy_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// LOOM_OP_CHANNEL_SELECT: Wait for any ready incoming endpoint of a relation-bound channel and accept one record. The source rank identifies the producer in the channel's source group; the returned read owns that record until it is released or transferred.
// %source_rank, %read = channel.select %progress : channel<tile<1xi32>> -> (index, read<tile<1xi32>>)
LOOM_DEFINE_ISA(loom_channel_select_isa, LOOM_OP_CHANNEL_SELECT)
LOOM_DEFINE_OPERAND(loom_channel_select_channel, 0)
LOOM_DEFINE_RESULT(loom_channel_select_source_rank, 0)
LOOM_DEFINE_RESULT(loom_channel_select_read, 1)
iree_status_t loom_channel_select_build(
    loom_builder_t* builder,
    loom_may_consume loom_value_id_t channel,
    loom_type_t source_rank_type,
    loom_type_t read_type,
    loom_location_id_t location,
    loom_op_t** out_op);
iree_status_t loom_channel_select_facts(
    loom_fact_context_t* context,
    const loom_module_t* module, const loom_op_t* op,
    const loom_value_facts_t* operand_facts,
    loom_value_facts_t* result_facts);
iree_status_t loom_channel_select_verify(
    const loom_module_t* module, const loom_op_t* op,
    iree_diagnostic_emitter_t emitter);

// Returns the vtable array for the channel dialect.
const loom_op_vtable_t* const* loom_channel_dialect_vtables(
    iree_host_size_t* out_count);

// Returns the dense semantic metadata array for the channel dialect.
const loom_op_semantics_t* loom_channel_dialect_op_semantics(
    iree_host_size_t* out_count);

// Returns semantic metadata for a channel op kind, or empty metadata.
loom_op_semantics_t loom_channel_op_semantics(
    loom_op_kind_t kind);

#ifdef __cplusplus
}
#endif

// Additional named attribute helpers are generated with this dialect.
#include "loom/ops/channel/ops.inc"

#endif  // LOOM_OPS_CHANNEL_OPS_H_
