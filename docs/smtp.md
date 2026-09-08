# SMTP — outbound mail for FlyNAS

*Written 2026-09-07, after the VaultWarden recipe forced the issue.*

## The short version

SMTP is **already implemented and unconfigured** — it is not a missing feature.
`util/smtp.lua` speaks SMTP, `api/settings.lua` exposes get/set/test, the config
lives in the `config` table under key `smtp`, and `monitor_worker.lua` already
uses it for alerts. What is missing is (a) a UI to configure it, (b) a provider
decision, and (c) several fixes in the client before it is worth trusting.

The blocker is not Gmail. It is that nothing can *reach* the settings, and the
client has a few defects that matter more than which provider you pick.

## Why this became urgent

VaultWarden keys accounts by email and refuses one the identity provider has
not marked verified. FlyNAS marks nothing verified, because
`PUT /api/users/:id/email` sets `email_verified = 0` and calls
`email_auth.send_code`, which fails with `SMTP not configured`. The VaultWarden
recipe therefore ships `SSO_ALLOW_UNKNOWN_EMAIL_VERIFICATION=true` — a real
relaxation, an account bound to an address nobody proved belongs to the user.
Every remaining SSO app (Seafile next) will inherit the same workaround unless
this is fixed. Turning that flag off is the acceptance test for this work.

Second consumer: monitor alerts (`monitor_worker.lua`) silently do not fire
without SMTP, which is a poor property for a NAS.

## Gmail: the honest answer

**Do not plan on sending as Gmail.** Two paths exist and both are bad for a
self-hosted appliance:

1. **App password + `AUTH LOGIN`.** This is the only Gmail path the current
   client could use at all. If Google has withdrawn app passwords for your
   account type, this is simply closed. *I could not verify the current status
   from this environment — the sandbox proxy blocks Google's documentation
   (`support.google.com` returns no response) — so confirm before relying on
   either answer.* Note it has always required 2-Step Verification, and
   "less secure app access" is long gone.

2. **XOAUTH2.** Not implemented (`util/smtp.lua` does `AUTH LOGIN` only), and
   implementing it does not actually rescue you. Sending mail needs a
   *restricted* scope, so a self-hosted app must either stay in Google Cloud
   "Testing" publishing status — where refresh tokens expire after seven days,
   meaning someone re-authorises the NAS every week — or pass Google's app
   verification and security assessment, which is not a realistic ask for a
   personal appliance. This is why "just add XOAUTH2" is not the fix it looks
   like.

**Workarounds that do work, in order of preference:**

- **Relay through a transactional provider, keep Gmail as the destination.**
  Nothing stops FlyNAS mailing *to* a Gmail address; the constraint is only on
  authenticating *as* one. This is the recommended answer and needs no new code.
- **Google Workspace SMTP relay** (`smtp-relay.gmail.com`), if you have
  Workspace. It authenticates the sending *IP* rather than a per-user password,
  which sidesteps app passwords entirely. Requires a Workspace admin, a static
  IP to allowlist, and it is not available to consumer Gmail accounts.
- **Gmail "Send mail as"** to keep a Gmail address in the From line while a
  provider does the sending. Needs a one-time verification in Gmail and the
  provider's domain to be authorised; cosmetic rather than structural.

## Recommended provider

**SMTP2GO** — it is already the house choice (`init.lua` describes RoundCube as
"Webmail (with smtp2go relay)"), so picking it keeps one provider across the
appliance. It hands out an ordinary SMTP username/password, which is exactly
what the existing client speaks, and its free tier is far above what FlyNAS
needs — verification codes and monitor alerts, not bulk mail.

Anything offering plain SMTP credentials works equally well: Resend, Postmark,
Mailgun, Brevo, Amazon SES, Fastmail, Migadu, mailbox.org.

**Do not** run a local Postfix as the default. Residential IPs are blocked or
greylisted nearly everywhere, port 25 outbound is commonly blocked by ISPs, and
there is no PTR record to fix it with. It is a support burden that looks free.

⚠️ **One provider detail interacts with a client bug:** SMTP2GO's alternative
port **2525** would get *no TLS at all* from the current client, which decides
encryption from the port number (see defect 5). Use 587 until that is fixed.

## Defects to fix before trusting this

Found by reading `util/smtp.lua` and `api/users.lua`; ordered by severity.

1. **TLS certificates are not verified.** `sock:sslhandshake(nil, config.host,
   false)` — the third argument is `ssl_verify`. The SMTP password is then
   handed to whoever answers. Fix: pass `true` and point
   `lua_ssl_trusted_certificate` at the system CA bundle. **Security, must fix.**

2. **No address or header sanitisation.** `PUT /api/users/:id/email` validates
   nothing — `body.email` goes straight to the DB, then into `RCPT TO:<...>`
   and the `To:` header. A CRLF in an address injects SMTP commands or headers.
   `subject` is interpolated the same way. Fix: validate the address, and
   reject any `\r`/`\n` in every interpolated field. **Security, must fix.**

3. **No `Date:` or `Message-ID:` header.** Both are effectively mandatory for
   deliverability; some receivers reject outright, many score it as spam. This
   alone can make a correctly configured provider look broken.

4. **No dot-stuffing.** The body is sent followed by `\r\n.\r\n`, so a body line
   consisting of a single `.` truncates the message. Verification codes are
   numeric and safe, but monitor alert bodies are arbitrary.

5. **TLS is chosen by port number.** 465 gets implicit TLS, 587 gets STARTTLS,
   and `config.tls` is consulted *only* on 587. Every other port — 25, 2525 —
   silently sends credentials in the clear. Fix: drive it from `config.tls`
   (`none` / `starttls` / `implicit`) with the port as a default hint.

6. **`AUTH LOGIN` only, and no capability check.** Some providers advertise only
   `AUTH PLAIN`. The client does not parse the EHLO response, so it cannot tell
   and fails with a confusing error. Add PLAIN and pick from the advertised set.

7. **Sent synchronously in the request path.** `send_code` runs inside the API
   request with a 10s socket timeout, so a slow provider becomes a slow API. Move
   to `ngx.timer.at` with a small retry, and stop losing the code when a
   transient failure returns a warning the UI shows once.

## Plan

**Phase 1 — harden the client.** Defects 1 and 2 first (they are security),
then 3–5. Small, self-contained changes to `util/smtp.lua` plus address
validation in `api/users.lua`. This is the part that must happen regardless of
provider.

**Phase 2 — Settings UI.** The Settings page is the last placeholder in the UI
(`docs/plan_july.md` §4), and `/api/settings/smtp` + `/api/settings/smtp/test`
already exist behind it. Host, port, TLS mode, username, password, From, and a
"Send test email" button wired to the existing test endpoint. Until this lands,
SMTP can only be configured by writing JSON into the `config` table by hand,
which is why it has stayed unconfigured.

**Phase 3 — close the verification loop.** With mail working, verify the admin
address end to end (`POST /api/users/:id/email/verify`), confirm FlyNAS emits
`email_verified: true` in the OIDC claims, then **remove
`SSO_ALLOW_UNKNOWN_EMAIL_VERIFICATION=true` from the VaultWarden recipe and
re-provision.** That is the acceptance test for this whole document.

**Phase 4 — make it reliable.** Defect 7: queue and retry. Then confirm monitor
alerts actually deliver, which nobody has ever seen.

## Testing

`POST /api/settings/smtp/test` already exists and is the fastest loop. Beyond
it: verify TLS is genuinely negotiated (not just that mail arrived), check that
a `.`-only body line survives, confirm `Date`/`Message-ID` are present in the
received headers, and confirm an address containing `\r\n` is rejected rather
than sent.

## Open questions

- **Do app passwords still work for the account you intend to use?** Could not
  be checked from here. It changes nothing about the recommendation — relay
  through a provider either way — but it decides whether Gmail is usable at all
  as a fallback.
- Per-user From addresses, or one appliance-wide sender? The client takes a
  single `config.from` today.
- Should a missing SMTP config be a visible warning in the UI? Right now
  verification and alerts both fail quietly, which is how this went unnoticed.
