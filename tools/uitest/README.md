# FlyNAS headless UI tests

Puppeteer-core driving system Chrome against the live h2dev VM.

```sh
npm install puppeteer-core     # in this directory, once
./run-all.sh                   # the whole safe suite: one tunnel, one TOTP
```

`run-all.sh` is the way in. It sources `../../bin/_common.sh`, so the
tunnel and every DB probe use the harness's SSH identity — host, port
2322 and key resolved by `hammer2-raid6/vmenv.sh`, **not** an
`~/.ssh/config` alias. That matters inside the claude-box sandbox, which
has no `~/.ssh` at all; see `hammer2-raid6/harness/README.md`. It also
exports `FLYNAS_SSH` (the whole ssh invocation as one string) for the
node tests that read the DB.

To drive one test by hand, keep the tunnel in the same shell — the
`ssh -f -L 8443` form drops intermittently in some environments and
surfaces as bogus "page not loaded" / "element not found" failures:

```sh
REPO="$(cd ../.. && pwd)"; . "$REPO/bin/_common.sh"
TOTP_SECRET=$(vssh sh <<'EOF'
sqlite3 /usr/local/flynas/flynas.db "SELECT totp_secret FROM users WHERE is_admin=1 LIMIT 1"
EOF
)
ssh $VM_SSH_OPTS -N -L 8443:localhost:443 "$VM_TARGET" & T=$!
sleep 3
TOTP_SECRET=$TOTP_SECRET node test-vms.mjs
kill $T
```

Background the `ssh` line directly rather than `vssh`: backgrounding a
shell *function* gives you the pid of a subshell, and killing that leaves
the real tunnel running.

Don't `pkill -f 'ssh … 8443'` to recycle the tunnel — the pattern
matches the killer's own command line and takes out the shell
(exit 144). Gate on `curl -ks https://localhost:8443/api/health`.

Each test sets `window.__flynasPauseRefresh = true` after login so the
pages' 5s auto-refresh can't rebuild the DOM mid-click/keystroke
(otherwise multi-step forms flake). State still updates after actions
(mutations reload explicitly). Form-fill steps also settle ~300ms
between fields, and lifecycle buttons retry via `clickUntil`.

Preconditions (seed via API before running, clean up after):

- test-accounts: user `testuser1` + group `testgrp` must exist; user
  `uitestuser` must not.
- test-storage: volume `tank` on vbd1+vbd2 must exist; vbd3/vbd4 free.
  Remove all volumes afterwards — fstab entries pointing at harness
  scratch disks break the next boot when the harness regenerates them.
- test-network: none (restores DHCP/UTC/NTP-disabled itself; never
  fires the armed network Apply).
- test-monitoring: none (creates + deletes its own monitor `uitest-mon`
  and channel `uitest-chan`). Cleanest with an empty monitors table.
  The page live-refreshes every 5s, so the test settles ~800ms between
  click and assertion to avoid clicking during a refresh.
- test-vms: none (creates + deletes its own VM `uitestvm`, 1 GB disk on
  the system dir, started under QEMU/NVMM; also toggles the NAT network
  and adds/removes a port-forward, so `dnsmasq` must be installed).
  Cleanest with an empty vms/port_forwards table, no orphan
  `uitestvm.img`, and the NAT bridge down. `findText` matches input
  echoes too, so assert on row-only text (the `Start` button), not the VM
  name. The page debounces its 5s refresh on interaction, but form-fill
  steps still settle ~300ms between fields so focus/keys register.
- test-apps: none (installs Seafile as VM `seafiletest`, then deletes it).
  Cleanest with an empty vms table and no orphan `seafiletest.img`.
  Installs the 7th catalog Install button (alphabetical: Seafile).

- test-oidc: none (run via `sh run-oidc.sh`, which seeds + cleans up its
  own `uitest-oidc` oidc_client). Drives the §2.10 SSO login `return`-param
  handoff end-to-end: logged-out `/api/oidc/authorize` → bounce → TOTP login
  → `consumeOidcReturn()` replays authorize → app callback with `code`+`state`;
  also asserts the open-redirect guard rejects `?return=//evil`.

TOTP gotcha: `totp()` is computed on the **host** clock, but the server
validates against the **VM** clock. The h2dev VM drifts (no steady NTP); >~30s
skew makes every login fail with "invalid code". Before a run, sync it:
`../../bin/exec "date -u $(date -u +%Y%m%d%H%M.%S)"`.

Notes:

- Clay only registers a press if the mouse button is held across a
  rAF frame — always click with `{ delay: 120 }`, plain clicks are
  silently dropped.
- Clay renders text as absolutely-positioned `div.text` elements;
  `findText()` locates them by exact content for clicking/asserting.
- Tests mutate the VM (users, groups, volumes) and clean up best-
  effort via the UI itself; check leftover state if a run dies midway.
