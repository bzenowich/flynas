# FlyNAS headless UI tests

Puppeteer-core driving system Chrome against the live h2dev VM.

Setup:

```sh
npm install puppeteer-core            # in this directory
ssh -N -L 8443:localhost:443 h2dev &  # tunnel to FlyNAS HTTPS
TOTP_SECRET=$(ssh h2dev sqlite3 /usr/local/flynas/flynas.db \
    "\"SELECT totp_secret FROM users WHERE is_admin=1 LIMIT 1\"")
TOTP_SECRET=$TOTP_SECRET node test-accounts.mjs
TOTP_SECRET=$TOTP_SECRET node test-storage.mjs
TOTP_SECRET=$TOTP_SECRET node test-network.mjs
TOTP_SECRET=$TOTP_SECRET node test-monitoring.mjs
TOTP_SECRET=$TOTP_SECRET node test-vms.mjs
TOTP_SECRET=$TOTP_SECRET node test-apps.mjs
```

The `ssh -f -L 8443` tunnel drops intermittently in some environments
and surfaces as bogus "page not loaded" / "element not found" failures.
For reliable runs keep the tunnel in the same shell as the test:

```sh
ssh -N -L 8443:localhost:443 h2dev & T=$!
sleep 3
TOTP_SECRET=$TOTP_SECRET node test-vms.mjs
kill $T
```

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

Notes:

- Clay only registers a press if the mouse button is held across a
  rAF frame — always click with `{ delay: 120 }`, plain clicks are
  silently dropped.
- Clay renders text as absolutely-positioned `div.text` elements;
  `findText()` locates them by exact content for clicking/asserting.
- Tests mutate the VM (users, groups, volumes) and clean up best-
  effort via the UI itself; check leftover state if a run dies midway.
