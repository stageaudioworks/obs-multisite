// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// relay_send.h — the send path's names, re-exported for the relay.
//
// The straight-copy sender — the destination model, the ffmpeg plan and the
// ffmpeg child — moved up into src/core so the appliance encoder can reuse it
// rather than keep a second copy (the relay is a consumer of the core, never
// the other way round, which its own CMake has always said). The relay's code
// is unchanged: this header pulls the core names back into the relay's own
// namespace, so a file that used to include destination.h, stream_plan.h or
// ffmpeg_process.h includes this instead and reads exactly as it did.
#include "destination.h"
#include "ffmpeg_process.h"
#include "stream_plan.h"

namespace multisite_relay {

using multisite::AudioSelection;
using multisite::Destination;
using multisite::FfmpegProcess;
using multisite::Protocol;
using multisite::RoomSendability;
using multisite::SrtMode;
using multisite::StreamPlan;
using multisite::affects_stream;
using multisite::ffmpeg_supports_srt;
using multisite::is_listener;
using multisite::normalize;
using multisite::output_url;
using multisite::plan_stream;
using multisite::protocol_of;
using multisite::redact;
using multisite::sendability;
using multisite::validate;

} // namespace multisite_relay
