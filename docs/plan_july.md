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

### 2. Quick fixes noted in step 7 — do before the recipe fan-out

- **dnsmasq reservation race** — a guest got a dynamic IP despite a correct
  reservation. Bites every app install; deterministic IPs matter more with 8
  more recipes coming.
- **SameSite=Strict → Lax** on the session cookie — cross-host SSO re-login
  breaks in production; one-line fix, retest e2e.

### 3. Installer rework (step 13, marked ◐)

`install.sh` is stale vs. the current layout. Accumulated TODO list:

- Alpine base image fetch + `no_timer_check` patch (needs a Linux host —
  awkward; consider hosting a pre-patched image somewhere)
- `nvmm_load="YES"` in loader.conf
- OIDC keypair generation
- `seeds/` and `images/` directories
- dnsmasq setup
- pf anchors
- cron jobs

Worth doing soon — the gap between "works on h2dev" and "installable" grows
with each feature.

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

Do the two step-7 follow-up fixes first (small, correctness), then the
VaultWarden recipe (fast pattern confirmation), then Seafile (hard, high
value). Rework the installer once ~3 recipes are green — then the full
fresh-install path can be tested on a clean VM.
