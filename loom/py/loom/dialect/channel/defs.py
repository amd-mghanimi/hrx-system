# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Owned record access and communication, independent of execution placement."""

from loom.assembly import (
    ARROW,
    COLON,
    COMMA,
    LPAREN,
    RPAREN,
    OptionalGroup,
    Ref,
    ResultType,
    ResultTypeList,
    TemplateParam,
    TypeOf,
)
from loom.builtin_types import ChannelReadMode
from loom.dsl import (
    ANY,
    ATTR_TYPE_ENUM,
    INDEX,
    MEMORY_FENCE,
    UNIQUE_IDENTITY,
    VIEW,
    AliasResult,
    AttrDef,
    Borrow,
    BorrowedResult,
    Consume,
    Dialect,
    EnumCase,
    EnumDef,
    FreshResult,
    Op,
    Operand,
    ReadWrites,
    Release,
    Result,
)

channel_ops = Dialect(
    "channel",
    dialect_id=0x24,
    doc="Typed communication with explicit record ownership and payload readiness.",
)

ChannelDiscipline = EnumDef(
    "ChannelDiscipline",
    [EnumCase("fifo", 1, doc="Records are reserved and accepted in FIFO order.")],
    doc="Admission and delivery discipline for a bound channel.",
)

channel_bind = Op(
    "channel.bind",
    group=channel_ops,
    doc=(
        "Bind a fresh channel identity to caller-owned record storage. The leading "
        "view dimension indexes slots; capacity bounds the admitted records. The "
        "caller retains the allocation through all accesses and transfers. Equal "
        "backing addresses do not make two bindings the same protocol."
    ),
    operands=[Operand("storage", VIEW), Operand("capacity", INDEX)],
    attrs=[AttrDef("discipline", ATTR_TYPE_ENUM, enum_def=ChannelDiscipline)],
    results=[Result("result", ANY)],
    traits=[UNIQUE_IDENTITY],
    verify="loom_channel_bind_verify",
    type_transfer="loom_channel_bind_type_transfer",
    facts="loom_channel_bind_facts",
    format=[TemplateParam("discipline"), Ref("storage"), COMMA, Ref("capacity"), COLON, TypeOf("storage"), COMMA, TypeOf("capacity"), ARROW, ResultType("result")],
    examples=["%channel = channel.bind<fifo> %storage, %capacity : view<2x144xi32>, index -> channel<tile<144xi32>>"],
)

channel_reserve = Op(
    "channel.reserve",
    group=channel_ops,
    doc=("Wait for writable storage admission and reserve one producer record. The payload view borrows the write obligation and is uninitialized."),
    operands=[Operand("channel", ANY)],
    results=[Result("write", ANY), Result("view", VIEW)],
    effects=[ReadWrites("channel")],
    ownership_effects=[Borrow("channel"), FreshResult("write"), BorrowedResult("view")],
    traits=[MEMORY_FENCE],
    verify="loom_channel_reserve_verify",
    type_transfer="loom_channel_selection_type_transfer",
    facts="loom_channel_access_facts",
    format=[Ref("channel"), COLON, TypeOf("channel"), ARROW, LPAREN, ResultType("write"), COMMA, ResultType("view"), RPAREN],
    examples=["%write, %view = channel.reserve %channel : channel<tile<144xi32>> -> (write<tile<144xi32>>, view<144xi32>)"],
)

channel_accept = Op(
    "channel.accept",
    group=channel_ops,
    doc=("Accept ownership of the next record without waiting for its payload. Mutable acceptance requires exclusive initialized storage at readiness."),
    operands=[Operand("channel", ANY)],
    attrs=[AttrDef("mode", ATTR_TYPE_ENUM, enum_def=ChannelReadMode, optional=True)],
    results=[Result("read", ANY)],
    effects=[ReadWrites("channel")],
    ownership_effects=[Borrow("channel"), FreshResult("read")],
    verify="loom_channel_accept_verify",
    facts="loom_channel_access_facts",
    format=[OptionalGroup([TemplateParam("mode")], anchor="mode"), Ref("channel"), COLON, TypeOf("channel"), ARROW, ResultType("read")],
    examples=["%read = channel.accept %channel : channel<tile<144xi32>> -> read<tile<144xi32>>"],
)

channel_wait = Op(
    "channel.wait",
    group=channel_ops,
    doc=("Wait for payload readiness and acquire its visibility before returning a borrowed view. The read remains owned. An unused view does not erase the readiness or acquisition effect."),
    operands=[Operand("read", ANY)],
    results=[Result("view", VIEW)],
    effects=[ReadWrites("read")],
    ownership_effects=[Borrow("read"), AliasResult("view", "read")],
    traits=[MEMORY_FENCE],
    verify="loom_channel_wait_verify",
    type_transfer="loom_channel_wait_type_transfer",
    facts="loom_channel_access_facts",
    format=[Ref("read"), COLON, TypeOf("read"), ARROW, ResultType("view")],
    examples=["%view = channel.wait %read : read<tile<144xi32>> -> view<144xi32>"],
)

channel_acquire = Op(
    "channel.acquire",
    group=channel_ops,
    doc=(
        "Accept the next record and wait for its payload readiness. Mutable consumption permits reading and modifying the initialized payload exclusively until ownership is released or transferred."
    ),
    operands=[Operand("channel", ANY)],
    attrs=[AttrDef("mode", ATTR_TYPE_ENUM, enum_def=ChannelReadMode, optional=True)],
    results=[Result("read", ANY), Result("view", VIEW)],
    effects=[ReadWrites("channel")],
    ownership_effects=[Borrow("channel"), FreshResult("read"), BorrowedResult("view")],
    traits=[MEMORY_FENCE],
    verify="loom_channel_acquire_verify",
    type_transfer="loom_channel_selection_type_transfer",
    facts="loom_channel_access_facts",
    format=[OptionalGroup([TemplateParam("mode")], anchor="mode"), Ref("channel"), COLON, TypeOf("channel"), ARROW, LPAREN, ResultType("read"), COMMA, ResultType("view"), RPAREN],
    examples=["%read, %view = channel.acquire<mutable> %channel : channel<tile<144xi32>> -> (read<tile<144xi32>, mutable>, view<144xi32>)"],
)

channel_publish = Op(
    "channel.publish",
    group=channel_ops,
    doc="Consume an initialized producer reservation and publish its payload with the binding's required visibility.",
    operands=[Operand("write", ANY)],
    effects=[ReadWrites("write")],
    ownership_effects=[Consume("write")],
    traits=[MEMORY_FENCE],
    verify="loom_channel_publish_verify",
    format=[Ref("write"), COLON, TypeOf("write")],
    examples=["channel.publish %write : write<tile<144xi32>>"],
)

channel_release = Op(
    "channel.release",
    group=channel_ops,
    doc=("Retire one consuming obligation after its payload uses. Reuse also requires retirement of every other reader or transfer obligation."),
    operands=[Operand("read", ANY)],
    effects=[ReadWrites("read")],
    ownership_effects=[Release("read")],
    traits=[MEMORY_FENCE],
    verify="loom_channel_release_verify",
    format=[Ref("read"), COLON, TypeOf("read")],
    examples=["channel.release %read : read<tile<144xi32>>"],
)

channel_fanout = Op(
    "channel.fanout",
    group=channel_ops,
    doc=(
        "Transfer one immutable read into two or more independently retiring "
        "reads of the same record. This implies neither a payload copy nor a "
        "runtime reference count. Mutable reads cannot be fanned out."
    ),
    operands=[Operand("read", ANY)],
    results=[Result("reads", ANY, variadic=True)],
    ownership_effects=[Consume("read"), FreshResult("reads")],
    verify="loom_channel_fanout_verify",
    facts="loom_channel_access_facts",
    format=[Ref("read"), COLON, TypeOf("read"), ARROW, ResultTypeList("reads")],
    examples=["%dma, %history = channel.fanout %read : read<tile<144xi32>> -> (read<tile<144xi32>>, read<tile<144xi32>>)"],
)

channel_copy = Op(
    "channel.copy",
    group=channel_ops,
    doc=(
        "Transfer both accesses to asynchronous transport. Retire the source "
        "after its last source read, independently of publishing the destination "
        "after completed writes and required visibility. The enclosing execution "
        "owns the transfer even after the issuing helper returns."
    ),
    operands=[Operand("source", ANY), Operand("destination", ANY)],
    effects=[ReadWrites("source"), ReadWrites("destination")],
    ownership_effects=[Consume("source"), Consume("destination")],
    verify="loom_channel_copy_verify",
    format=[Ref("source"), ARROW, Ref("destination"), COLON, TypeOf("source"), COMMA, TypeOf("destination")],
    examples=["channel.copy %read -> %write : read<tile<144xi32>>, write<tile<144xi32>>"],
)

channel_select = Op(
    "channel.select",
    group=channel_ops,
    doc=(
        "Wait for any ready incoming endpoint of a relation-bound channel and "
        "accept one record. The source rank identifies the producer in the "
        "channel's source group; the returned read owns that record until it "
        "is released or transferred."
    ),
    operands=[Operand("channel", ANY)],
    results=[
        Result("source_rank", INDEX),
        Result("read", ANY),
    ],
    effects=[ReadWrites("channel")],
    ownership_effects=[Borrow("channel"), FreshResult("read")],
    traits=[MEMORY_FENCE],
    verify="loom_channel_select_verify",
    facts="loom_channel_select_facts",
    format=[
        Ref("channel"),
        COLON,
        TypeOf("channel"),
        ARROW,
        LPAREN,
        ResultType("source_rank"),
        COMMA,
        ResultType("read"),
        RPAREN,
    ],
    examples=["%source_rank, %read = channel.select %progress : channel<tile<1xi32>> -> (index, read<tile<1xi32>>)"],
)

ALL_CHANNEL_OPS = (
    channel_bind,
    channel_reserve,
    channel_accept,
    channel_wait,
    channel_acquire,
    channel_publish,
    channel_release,
    channel_fanout,
    channel_copy,
    channel_select,
)
