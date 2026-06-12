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
```

Notes:

- Clay only registers a press if the mouse button is held across a
  rAF frame — always click with `{ delay: 120 }`, plain clicks are
  silently dropped.
- Clay renders text as absolutely-positioned `div.text` elements;
  `findText()` locates them by exact content for clicking/asserting.
- Tests mutate the VM (users, groups, volumes) and clean up best-
  effort via the UI itself; check leftover state if a run dies midway.
