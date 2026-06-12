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
```

Preconditions (seed via API before running, clean up after):

- test-accounts: user `testuser1` + group `testgrp` must exist; user
  `uitestuser` must not.
- test-storage: volume `tank` on vbd1+vbd2 must exist; vbd3/vbd4 free.
  Remove all volumes afterwards — fstab entries pointing at harness
  scratch disks break the next boot when the harness regenerates them.
- test-network: none (restores DHCP/UTC/NTP-disabled itself; never
  fires the armed network Apply).

Notes:

- Clay only registers a press if the mouse button is held across a
  rAF frame — always click with `{ delay: 120 }`, plain clicks are
  silently dropped.
- Clay renders text as absolutely-positioned `div.text` elements;
  `findText()` locates them by exact content for clicking/asserting.
- Tests mutate the VM (users, groups, volumes) and clean up best-
  effort via the UI itself; check leftover state if a run dies midway.
