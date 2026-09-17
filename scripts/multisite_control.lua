-- multisite_control.lua — OPTIONAL. Drives the obs-multisite output from Lua.
--
-- NOT REQUIRED any more: the Multisite Encoder dock does everything this does,
-- with saved settings, encoder selection and a live status readout. Use the
-- dock unless you have a reason not to.
--
-- Kept because a script is still the easier route for:
--   * a build without the Qt docks (see ENABLE_QT in CMakeLists.txt),
--   * starting a broadcast from other automation, or
--   * machines where the operator never opens the OBS window.
--
-- Note it does not expose everything the dock does — encoder choice and packed
-- channel labels are dock-only, and this script always uses x264.
--
-- Install: Tools → Scripts → + → select this file, fill in the fields, then use
-- the "Go live" / "End broadcast" buttons.

obs = obslua

local output      = nil
local venc        = nil
local aencs       = {}
local live        = false

local cfg = {
  endpoint_host = "",
  account_id    = "",
  bucket        = "",
  access_key    = "",
  secret_key    = "",
  region        = "auto",
  room_id       = "main-auditorium",
  seg_dur       = 6.0,
  track_labels  = "Main mix,Sermon ISO,Click",
  audio_tracks  = 1,
  vbitrate      = 6000,
  abitrate      = 160,
}

function script_description()
  return [[<b>Multisite output control</b><br/>
Publishes this OBS instance's program feed as CMAF segments to an
S3-compatible bucket (Cloudflare R2, S3, Garage...) with a durable
store-and-forward queue.<br/><br/>
Fill in the storage details, choose how many audio tracks to send, then
click <b>Go live</b>. Watch the OBS log for <code>[multisite]</code> lines.]]
end

function script_properties()
  local p = obs.obs_properties_create()

  obs.obs_properties_add_text(p, "account_id", "R2 Account ID (or blank)", obs.OBS_TEXT_DEFAULT)
  obs.obs_properties_add_text(p, "endpoint_host", "Endpoint host (blank for R2)", obs.OBS_TEXT_DEFAULT)
  obs.obs_properties_add_text(p, "bucket", "Bucket", obs.OBS_TEXT_DEFAULT)
  obs.obs_properties_add_text(p, "access_key", "Access Key ID", obs.OBS_TEXT_DEFAULT)
  obs.obs_properties_add_text(p, "secret_key", "Secret Access Key", obs.OBS_TEXT_PASSWORD)
  obs.obs_properties_add_text(p, "region", "Region ('auto' for R2)", obs.OBS_TEXT_DEFAULT)
  obs.obs_properties_add_text(p, "room_id", "Room ID", obs.OBS_TEXT_DEFAULT)
  obs.obs_properties_add_float_slider(p, "seg_dur", "Segment duration (s)", 2.0, 15.0, 0.5)

  obs.obs_properties_add_int_slider(p, "audio_tracks", "Audio tracks to send", 1, 6, 1)
  obs.obs_properties_add_text(p, "track_labels", "Track labels (comma separated)", obs.OBS_TEXT_DEFAULT)

  obs.obs_properties_add_int(p, "vbitrate", "Video bitrate (kbps)", 1000, 50000, 500)
  obs.obs_properties_add_int(p, "abitrate", "Audio bitrate per track (kbps)", 64, 320, 32)

  obs.obs_properties_add_button(p, "go_live", "Go live", go_live)
  obs.obs_properties_add_button(p, "end_live", "End broadcast", end_live)
  obs.obs_properties_add_button(p, "show_status", "Log status", log_status)

  return p
end

function script_defaults(s)
  obs.obs_data_set_default_string(s, "region", "auto")
  obs.obs_data_set_default_string(s, "room_id", "main-auditorium")
  obs.obs_data_set_default_double(s, "seg_dur", 6.0)
  obs.obs_data_set_default_int(s, "audio_tracks", 1)
  obs.obs_data_set_default_string(s, "track_labels", "Main mix,Sermon ISO,Click")
  obs.obs_data_set_default_int(s, "vbitrate", 6000)
  obs.obs_data_set_default_int(s, "abitrate", 160)
end

function script_update(s)
  cfg.account_id    = obs.obs_data_get_string(s, "account_id")
  cfg.endpoint_host = obs.obs_data_get_string(s, "endpoint_host")
  cfg.bucket        = obs.obs_data_get_string(s, "bucket")
  cfg.access_key    = obs.obs_data_get_string(s, "access_key")
  cfg.secret_key    = obs.obs_data_get_string(s, "secret_key")
  cfg.region        = obs.obs_data_get_string(s, "region")
  cfg.room_id       = obs.obs_data_get_string(s, "room_id")
  cfg.seg_dur       = obs.obs_data_get_double(s, "seg_dur")
  cfg.audio_tracks  = obs.obs_data_get_int(s, "audio_tracks")
  cfg.track_labels  = obs.obs_data_get_string(s, "track_labels")
  cfg.vbitrate      = obs.obs_data_get_int(s, "vbitrate")
  cfg.abitrate      = obs.obs_data_get_int(s, "abitrate")
end

local function log(fmt, ...)
  obs.script_log(obs.LOG_INFO, string.format(fmt, ...))
end

function log_status()
  if live then
    log("multisite: LIVE (room=%s, %d audio track(s))", cfg.room_id, cfg.audio_tracks)
    if output ~= nil then
      log("bytes uploaded: %d", obs.obs_output_get_total_bytes(output))
    end
  else
    log("multisite: idle")
  end
  log("check the OBS log for [multisite] lines from the plugin itself")
end

function go_live()
  if live then log("already live"); return end

  if cfg.bucket == "" or (cfg.account_id == "" and cfg.endpoint_host == "") then
    log("ERROR: set Bucket, and either R2 Account ID or Endpoint host")
    return
  end

  local s = obs.obs_data_create()
  obs.obs_data_set_string(s, "r2_account_id", cfg.account_id)
  obs.obs_data_set_string(s, "endpoint_host", cfg.endpoint_host)
  obs.obs_data_set_string(s, "bucket", cfg.bucket)
  obs.obs_data_set_string(s, "access_key_id", cfg.access_key)
  obs.obs_data_set_string(s, "secret_access_key", cfg.secret_key)
  obs.obs_data_set_string(s, "region", cfg.region)
  obs.obs_data_set_string(s, "room_id", cfg.room_id)
  obs.obs_data_set_double(s, "segment_duration_s", cfg.seg_dur)
  obs.obs_data_set_string(s, "track_labels", cfg.track_labels)
  obs.obs_data_set_bool(s, "send_expiry_tag", false)  -- R2 rejects tagging

  output = obs.obs_output_create("multisite_output", "multisite_out", s, nil)
  obs.obs_data_release(s)
  if output == nil then
    log("ERROR: could not create multisite_output (is the plugin loaded?)")
    return
  end

  -- Video encoder. Keyframe interval MUST be <= segment duration, or the
  -- muxer can never cut a segment.
  local vs = obs.obs_data_create()
  obs.obs_data_set_int(vs, "bitrate", cfg.vbitrate)
  obs.obs_data_set_int(vs, "keyint_sec", math.floor(cfg.seg_dur + 0.5))
  obs.obs_data_set_string(vs, "rate_control", "CBR")
  -- Disable scene-cut IDRs so keyframes land exactly on the segment interval;
  -- otherwise x264 inserts extra keyframes and segment lengths vary.
  obs.obs_data_set_string(vs, "x264opts", "scenecut=0")
  venc = obs.obs_video_encoder_create("obs_x264", "multisite_v", vs, nil)
  obs.obs_data_release(vs)
  obs.obs_encoder_set_video(venc, obs.obs_get_video())
  obs.obs_output_set_video_encoder(output, venc)

  -- One AAC encoder per requested track, bound to OBS mixer tracks 1..N.
  aencs = {}
  for i = 1, cfg.audio_tracks do
    local as = obs.obs_data_create()
    obs.obs_data_set_int(as, "bitrate", cfg.abitrate)
    local enc = obs.obs_audio_encoder_create("ffmpeg_aac", "multisite_a" .. i,
                                             as, i - 1, nil)
    obs.obs_data_release(as)
    obs.obs_encoder_set_audio(enc, obs.obs_get_audio())
    obs.obs_output_set_audio_encoder(output, enc, i - 1)
    aencs[i] = enc
  end

  if obs.obs_output_start(output) then
    live = true
    log("multisite: going live — room=%s, %d audio track(s), %.1fs segments",
        cfg.room_id, cfg.audio_tracks, cfg.seg_dur)
    log("watch the OBS log for [multisite] upload activity")
  else
    log("ERROR: obs_output_start failed: %s", obs.obs_output_get_last_error(output) or "unknown")
    release_all()
  end
end

function end_live()
  if not live then log("not live"); return end
  log("multisite: ending broadcast (draining upload queue...)")
  obs.obs_output_stop(output)
  live = false
  release_all()
  log("multisite: stopped")
end

function release_all()
  for i, enc in pairs(aencs) do
    if enc ~= nil then obs.obs_encoder_release(enc) end
  end
  aencs = {}
  if venc ~= nil then obs.obs_encoder_release(venc); venc = nil end
  if output ~= nil then obs.obs_output_release(output); output = nil end
end

function script_unload()
  if live and output ~= nil then obs.obs_output_stop(output) end
  release_all()
end
