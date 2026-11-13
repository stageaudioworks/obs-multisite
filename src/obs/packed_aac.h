// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace multisite_obs {

// Registers "multisite_packed_aac" with libobs: an AAC audio encoder whose
// eight channels are coded OCTAGONAL rather than 7.1. See packed_aac.cpp and
// BUGS.md entry 3. Called once, from obs_module_load().
void register_packed_aac();

} // namespace multisite_obs
