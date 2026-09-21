# LAN / direct delivery — design rationale (archive)

The rationale, history and measurements behind `PROJECT-SCOPE.md` §8.7, whose
short, specification-only form now lives there. This file keeps the full
reasoning verbatim, including the correction found while building the encoder
half, the `NullTransport` explanation, the `LanObjectServer`/`route_prefix`/
`SegmentCache`/five-hooks detail, and the `LanTransport`/`FallbackTransport`
internals. Read this before changing LAN serving, discovery or fallback.

> As it stood on 2026-09-21.

---

## 8.7 LAN / direct delivery

**Status: built, both halves, and every kind of receiver.** A receiver —
the OBS decoder plugin, the Raspberry Pi appliance, or the simulcast relay —
on the same network as the encoder, or reachable over an existing
site-to-site VPN, downloads directly from it: manifest, init segment, media
fragments and markers, instead of from the bucket, automatically preferring
that path when it answers and falling back to cloud, per request, when it
doesn't. Cloud delivery can also be turned off entirely for an operator who
wants everything to stay on one network and never touch a bucket at all. The
appliance's `Config`/`Player` (`src/appliance/config.h`, `player.h/.cpp`)
and the relay's `ConfigStore`/`RoomFeeder` (`relay/src/config_store.h`,
`room_feeder.h/.cpp`) both gained the identical `lan_host`/`lan_port`/
`lan_auth_token` fields and the same `LanTransport`/`FallbackTransport`
wiring as the OBS decoder — one codebase (`src/core/`), three independent
settings surfaces, no divergence in behaviour. The relay's own past-events
browsing, download and rebroadcast stay cloud-only regardless of LAN
settings: they are `list()`-based, which `LanObjectServer` does not serve —
it only ever holds the one event currently in progress, the same reason a
satellite's event browser is cloud-only too. What follows is kept in its
original, before-the-fact form, with
corrections noted in place where building it changed something — the
reasoning here is still the reasoning for why it works the way it does.

Every campus today reaches the main site the same way, and only that way:
through the bucket, over whatever internet connection each site has. That is
correct and stays correct — it is the whole reason this project works on
mobile data and LEO satellite links that would defeat a direct stream. But a
campus on the same building network as the main site, or reachable over a
VPN the church already runs between sites, has a faster and cheaper path
sitting unused: the encoder machine itself.

**What this adds, and what it deliberately does not replace — unless told
to.** A satellite that can reach the encoder directly — over the LAN, or over
an existing site-to-site VPN — downloads from it instead of from the bucket,
while the encoder, by default, keeps uploading to the bucket exactly as it
does today, unconditionally. Nothing about §3's "decentralized, no control
plane" holds any less true for this: the encoder serves the *identical*
object shape (`manifest.json`, `event.json`, `init.mp4`,
`segments/{seq}.m4s`, and — for LAN satellites following the room rather
than a pinned event — `live.json`) a cloud decoder already reads. The media
path and the signaling path are still the same path; there is just a second,
local way to walk it.

Cloud upload staying on by default is what makes LAN mode safe to *attempt*
in the first place: a cloud-only decoder, a LAN decoder whose link just
dropped, and the archival recording all still depend on it running
regardless of who else is connected directly. But an operator who has no use
for a cloud copy at all — a single building, no remote viewers, no interest
in an off-site archive — can turn cloud delivery off entirely for an event.
Doing so hands `Session` a `NullTransport` (`src/core/null_transport.h`) in
place of the real `S3Transport`: every PUT reports instant success, so the
spool → retry-uploader → manifest pipeline runs exactly as it always has —
segments confirm immediately, the LAN hooks fire on schedule — and nothing
ever actually leaves the machine. `Session` cannot tell the difference,
which is the point: cloud-off is not a separate code path, it is the same
one pointed at a transport that keeps nothing. The dock refuses to go live
with both cloud and LAN off at once (there would be nowhere for anything to
go), and the checkbox for it only appears once LAN delivery is turned on.

**One correction from the original plan, found while building the encoder
half:** this was going to serve straight from "the same durable spool" a
cloud decoder's segments pass through. It cannot. The spool's entire job is
to hold a segment only until the bucket confirms it, then delete it (see
`spool_queue.h`) — which means the segment a LAN decoder is most likely to
actually want (recent, ordinary programme, already confirmed) is by design
the segment the spool no longer has. LAN serving keeps its own bounded
retention window instead — a `SegmentCache`, the exact same class a decoder
already uses for its own cache, fed via three new `Session` hooks
(`set_event_started_callback`, `set_segment_confirmed_callback`,
`set_manifest_published_callback`) fired at exactly the moments `begin_common()`,
`on_confirmed()` and `publish_manifest_locked()` already have the relevant
bytes or JSON in hand. `Session` itself stays completely unaware that LAN
delivery exists — the hooks cost nothing when unset, and it never holds a
reference to the class that uses them. The window's directory is the operator's
**Cache folder** with `lan_cache` beneath it, or the plugin's own config when
that is left blank, so the outgoing queue and the LAN copies sit in one place an
operator can find (and clear).

**Shape, as built.** The encoder's HTTP server (`src/core/http_server.h`)
gained `route_prefix()` — a "starts with", not "equals", route, matched
longest-prefix-first, needed because a segment's path names a sequence
number that cannot be registered as one exact route per possible value. A
new `LanObjectServer` (`src/core/lan_object_server.h`) combines that with a
`SegmentCache` and five `Session` hooks (`set_event_started_callback`,
`set_segment_confirmed_callback`, `set_manifest_published_callback`,
`set_live_published_callback`, `set_markers_published_callback`) into the
actual object server: `GET .../manifest.json`, `.../event.json`,
`.../init.mp4`, `.../segments/{seq}.m4s`, `.../markers.json`, and
`.../rooms/{room}/live.json` — all proven end to end over a real loopback
socket in `tests/test_lan_object_server.cpp`, including the retention cap,
an event switch discarding the previous event's window and markers, and
auth enforcement. `markers.json` earned its own hook rather than riding
along with the manifest: found live, once cloud delivery could actually be
turned off — without it, a marker dropped mid-event never reached a
LAN-only satellite at all, since there is no cloud copy to fall back to for
just that one object.

On the decoder side, `LanTransport` (`src/core/lan_transport.h`) implements
the same `Transport` interface a decoder already downloads through, as a
plain HTTP client against exactly those routes — proven against a real
`LanObjectServer` in `tests/test_lan_transport.cpp`, including what a
genuine miss (404, LAN path alive) looks like next to a connection failure
(LAN path itself down), which is what tells `FallbackTransport`
(`src/core/fallback_transport.h`) which one happened. That class is the
whole of "preference and fallback": it holds a LAN `Transport&` and a cloud
`Transport&` and, per `get()` call, tries LAN first and only reaches for
cloud if LAN didn't answer — so a segment that aged out of the LAN's bounded
retention window falls back to cloud for *that segment alone*, without
flipping the whole session to cloud over one old fragment. `DecoderSession`
is handed whichever of the two — or, for a LAN-only satellite with no cloud
credentials at all, `LanTransport` alone — it never learns which, the same
boundary `Session`'s hooks keep on the encoder side.

The Raspberry Pi appliance (`src/appliance/`) is the second satellite this
applies to, wired the same way: `Player::rebuild_session()` builds the same
LAN/cloud/fallback choice `multisite_source.cpp` does, `Config` carries the
matching three fields, and the web settings page (`web/index.html`,
`api.cpp`'s `/api/config`) is the appliance's equivalent of the decoder
dock's settings dialog. `Player::storage_health()` (`/api/storage`) reports
`lan_configured`/`lan_active` the same way the OBS decoder's status JSON
does, including for a LAN-only box with no cloud transport at all to ask
about — proven with a real `multisite-player` process pointed at a real
`LanObjectServer` (an OBS encoder with LAN on), which correctly reported
"room is LIVE" and served segments with no bucket involved at any point.

- **Discovery — built, and deliberately manual.** A host (and port, and an
  optional shared token) typed into the decoder dock's settings, the same
  place cloud credentials go — not auto-discovered. mDNS was considered and
  set aside: it does nothing for the VPN case, where the two ends are rarely
  on the same broadcast domain, and it is one more thing to fail silently on
  a locked-down church network. A satellite with a LAN host configured but no
  cloud credentials at all now works LAN-only — `DecoderSettings::configured()`
  accepts either, not just cloud — and one following the room (not a pinned
  past event) discovers the live event id from the LAN server's own
  `live.json`, needing no bucket at all when the encoder also has cloud
  delivery turned off.
- **Auth — built, still a shared secret, not yet paired.**
  `LanServerConfig::auth_token` / `LanTransportConfig::auth_token` and the
  `Authorization: Bearer <token>` check are real and enforced end to end;
  what generates that token and gets it onto a decoder is still typing the
  same string into both docks, not yet the device-code pairing flow §8.5
  designs for cloud credentials — the eventual goal is still one pairing flow
  an operator learns once, used for both. A plain LAN inside one building is
  already treated as the trust boundary elsewhere in this project (the
  remote-control pages have no password and no TLS, deliberately); a token
  matters more once the path crosses a VPN.
- **Preference and fallback — built, per request rather than per session.**
  See `FallbackTransport` above. Deliberately simpler than tracking
  `LinkHealth` hysteresis per transport and switching on a threshold: a
  per-request decision cannot get "stuck" preferring the wrong path, and it
  needs no timer, no state machine, and no operator-visible mode to explain.
- **Visibility — built.** The decoder dock's storage-link line names which
  path the most recent fetch actually took — *"via LAN"* or *"via cloud"* —
  next to the existing colo/throughput readout, and only appears at all once
  a LAN host is actually configured.

**Deferred rather than decided against.** Whether LAN mode serves a segment
the moment it's spooled (lower latency than cloud, since it skips waiting for
upload confirmation) or only once the bucket has confirmed it (identical
consistency guarantee to cloud, simpler to reason about) is left for a later,
explicitly opt-in mode. A first build serves only what's already confirmed —
the same manifest a cloud decoder would eventually see, just sooner.
