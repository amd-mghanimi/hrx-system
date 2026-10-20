# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Portable channel access types and separate payload views in text and bytecode."""

from loom.builtin_types import ALL_BUILTIN_TYPES
from loom.dialect.channel import ALL_CHANNEL_OPS
from loom.dialect.func import ALL_FUNC_OPS
from loom.dialect.index import ALL_INDEX_OPS
from loom.format.bytecode.reader import read_module
from loom.format.bytecode.writer import write_module
from loom.format.text.parser import Parser
from loom.format.text.printer import Printer


def test_owned_accesses_and_dynamic_payload_roundtrip() -> None:
    source = """func.def @forward(%width: index, %storage: view<2x[%width]xbf16>, %destination: write<tile<[%width]xbf16>>) {
  %capacity = index.constant 2 : index
  %channel = channel.bind<fifo> %storage, %capacity : view<2x[%width]xbf16>, index -> channel<tile<[%width]xbf16>>
  %read, %payload = channel.acquire %channel : channel<tile<[%width]xbf16>> -> (read<tile<[%width]xbf16>>, view<[%width]xbf16>)
  %transfer, %history = channel.fanout %read : read<tile<[%width]xbf16>> -> (read<tile<[%width]xbf16>>, read<tile<[%width]xbf16>>)
  channel.copy %transfer -> %destination : read<tile<[%width]xbf16>>, write<tile<[%width]xbf16>>
  %previous = channel.wait %history : read<tile<[%width]xbf16>> -> view<[%width]xbf16>
  channel.release %history : read<tile<[%width]xbf16>>
  %mutable = channel.accept<mutable> %channel : channel<tile<[%width]xbf16>> -> read<tile<[%width]xbf16>, mutable>
  channel.release %mutable : read<tile<[%width]xbf16>, mutable>
  %write, %output = channel.reserve %channel : channel<tile<[%width]xbf16>> -> (write<tile<[%width]xbf16>>, view<[%width]xbf16>)
  channel.publish %write : write<tile<[%width]xbf16>>
  %source_rank, %selected = channel.select %channel : channel<tile<[%width]xbf16>> -> (index, read<tile<[%width]xbf16>>)
  channel.release %selected : read<tile<[%width]xbf16>>
  func.return
}
"""
    ops = (*ALL_CHANNEL_OPS, *ALL_FUNC_OPS, *ALL_INDEX_OPS)
    parser = Parser()
    parser.register_ops(ops)
    parser.register_types(ALL_BUILTIN_TYPES)
    printer = Printer()
    printer.register_ops(ops)
    printer.register_types(ALL_BUILTIN_TYPES)
    module = parser.parse(source)
    assert printer.print_module(module) == source
    assert printer.print_module(read_module(write_module(module))) == source
