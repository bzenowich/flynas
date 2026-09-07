# FlyNAS — July 2026 Plan

*Written 2026-07-01, after §2.11 step 7 (full Forgejo SSO round-trip) went green.*

## Where the project stands

Phases 1–3 are done. Guest provisioning (§2.11) is proven end-to-end with
Forgejo: qcow2 overlay → cidata seed → cloud-init → Docker → app up → OIDC SSO
verified in a real browser. The pattern works; now scale it.

## Next steps, ordered

### 1. §2.11 step 8 — generalize recipes (main line of work)

Port the Forgejo pattern to the other 8 apps. Suggested order by
payoff/difficulty:

- **VaultWarden** — single container, OIDC-only; simplest next validation of
  the pattern.
- **Seafile** — flagship app (file sharing = NAS core value); multi-container
  (server + db), stress-tests the recipe pattern with compose. Also unblocks
  §4.3 group sync and `/api/restore/share`.
- **Jellyfin** — needs the SSO plugin baked in (known caveat); also the first
  app wanting volume passthrough for media — may force new design work
  (virtio-fs / 9p / NFS from host?).
- Then: Readeck, DokuWiki, Wekan, CryptPad, RoundCube (RoundCube = IMAP auth,
  no SSO — simplest recipe).

Each recipe: `lua/recipes/<app>.yaml` + `oidc_redirect_path` seed + verify
boot + SSO on h2dev.

### 2. Quick fixes noted in step 7 — **BOTH DONE (2026-09-07)**

- ~~**dnsmasq reservation race**~~ — **fixed.** Root cause was not SIGHUP
  timing but an overlapping pool: `dhcp-range=.50-.250` covered the reservation
  range, so losing the race produced a *valid* dynamic lease. Now
  `dhcp-range=10.77.0.0,static` (reservations only). `forge1` took its reserved
  `10.77.0.101` on first boot.
- ~~**SameSite=Strict → Lax**~~ — **fixed** and verified on the wire (the
  logout cookie had to change with it, or the browser keeps the original).

### 3. Installer rework (step 13) — **DONE and proven (2026-09-07)**

Reworked and exercised on a pristine guest: install → service up → admin →
Forgejo installed, provisioned and serving, `OIDC registration: ok`. The one
step not re-run is the browser SSO click (no Chrome in the sandbox — dangling
symlink into an unmounted `/opt/google`).

The list it was carrying, all now handled except where noted:

- Alpine base image fetch + `no_timer_check` patch — **still open.** The
  installer takes a pre-patched image via `FLYNAS_ALPINE_IMAGE` and warns when
  there is none; it cannot make one, since patching means writing ext4. Hosting
  a pre-patched image is still the way out.
- ~~`nvmm_load="YES"` in loader.conf~~ — done
- ~~OIDC keypair generation~~ — done
- ~~`seeds/` and `images/` directories~~ — done
- ~~dnsmasq setup~~ — done (package + conf ship in the overlay)
- ~~pf anchors~~ — done (modules loaded; the helper loads the anchor)
- ~~cron jobs~~ — done (hourly-snapshots; daily-scrub does not exist yet)

The gap between "works on h2dev" and "installable" is closed for now; it will
reopen with each feature, so re-run the fresh-install path periodically.

### 4. Cleanup of deferred small items (pick off between recipes)

- Dashboard monitor-summary card (§4.1, deferred since step 12)
- Settings page (the only placeholder page left) — natural home for
  `external_url` config, SSL cert upload, config import/export (§5.2)
- Monitor history sparkline + 90-day uptime grid

### 5. Longer-horizon (park, don't start)

- HAMMER2 snapshot-mount kernel panic — blocks point-in-time backup; kernel
  work
- Physical-NIC LAN bridge — real-hardware only
- Step 14 hardening pass

## Recommendation

*Superseded 2026-09-07: the two fixes and the installer are done, and the
fresh-install path has been tested on a clean VM.* Next is the recipe fan-out
itself — **VaultWarden** (single container, OIDC-only; note its
`oidc_redirect_path` is not seeded yet — `OIDC_REDIRECTS` in `init.lua` has
only Seafile and Forgejo), then **Seafile**, then **Jellyfin**.
