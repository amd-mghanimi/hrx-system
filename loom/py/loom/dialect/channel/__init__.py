# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Channel operations over builtin communication and owned-access types."""

from loom.dialect.channel.defs import (
    ALL_CHANNEL_OPS,
    ChannelDiscipline,
    channel_accept,
    channel_acquire,
    channel_bind,
    channel_copy,
    channel_fanout,
    channel_ops,
    channel_publish,
    channel_release,
    channel_reserve,
    channel_select,
    channel_wait,
)

__all__ = [
    "ALL_CHANNEL_OPS",
    "ChannelDiscipline",
    "channel_ops",
    "channel_bind",
    "channel_reserve",
    "channel_select",
    "channel_accept",
    "channel_wait",
    "channel_acquire",
    "channel_publish",
    "channel_release",
    "channel_fanout",
    "channel_copy",
]
