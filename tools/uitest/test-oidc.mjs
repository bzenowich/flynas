// FlyNAS OIDC SSO login-handoff UI test. Exercises §2.10's SPA
// `return`-param handoff end-to-end in a real browser:
//
//   1. (logged out) GET /api/oidc/authorize?... → 302 to /?return=<authorize>
//   2. SPA loads, user logs in with TOTP
//   3. enterMain() → consumeOidcReturn() → window.location.replace(authorize)
//   4. now we hold a session → authorize issues a code → 302 to redirect_uri
//   5. browser lands on the app callback carrying ?code=&state=
//
// The client's redirect_uri is same-origin (/oidc-cb-probe) so the final
// hop lands on a real (404) page whose URL still carries the grant — no DNS
// or request interception needed. Also asserts the open-redirect guard live:
// a malicious ?return=//evil must NOT navigate away.
//
// Seed/cleanup of the oidc_client is done over ssh by run-oidc.sh; this
// script only drives the browser. Needs TOTP_SECRET + an admin session.
import puppeteer from 'puppeteer-core';
import { createHmac } from 'crypto';

const SECRET = process.env.TOTP_SECRET;
const BASE = 'https://localhost:8443';
const CLIENT_ID = process.env.OIDC_CLIENT_ID || 'uitest-oidc';
const REDIRECT = BASE + '/oidc-cb-probe';
const STATE = 'uitest-state-7Z9';
const NONCE = 'uitest-nonce-42';

function b32decode(s) {
    const A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
    let bits = 0, value = 0, out = [];
    for (const c of s.replace(/=+$/, '')) {
        value = (value << 5) | A.indexOf(c);
        bits += 5;
        if (bits >= 8) { out.push((value >>> (bits - 8)) & 0xff); bits -= 8; }
    }
    return Buffer.from(out);
}

function totp(secret) {
    const t = Math.floor(Date.now() / 1000 / 30);
    const msg = Buffer.alloc(8);
    msg.writeBigUInt64BE(BigInt(t));
    const h = createHmac('sha1', b32decode(secret)).update(msg).digest();
    const o = h[19] & 15;
    return String((h.readUInt32BE(o) & 0x7fffffff) % 1000000).padStart(6, '0');
}

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

async function findText(page, text) {
    return page.evaluate((t) => {
        for (const el of document.querySelectorAll('div.text')) {
            if (el.textContent.trim() === t) {
                const r = el.getBoundingClientRect();
                return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
            }
        }
        return null;
    }, text);
}

const authorizeUrl = () => BASE + '/api/oidc/authorize?' + new URLSearchParams({
    response_type: 'code',
    client_id: CLIENT_ID,
    redirect_uri: REDIRECT,
    scope: 'openid profile groups',
    state: STATE,
    nonce: NONCE,
}).toString();

const browser = await puppeteer.launch({
    executablePath: '/usr/bin/google-chrome',
    headless: 'new',
    args: ['--no-sandbox', '--ignore-certificate-errors', '--window-size=1280,800'],
    defaultViewport: { width: 1280, height: 800 },
});
const page = await browser.newPage();
let failed = false;
try {
    // 1. Hit authorize while logged out. Backend has no session → 302 to the
    //    SPA login with ?return=<authorize>. networkidle settles on the SPA.
    await page.goto(authorizeUrl(), { waitUntil: 'networkidle0' });
    await sleep(500);

    const ret = await page.evaluate(() => new URLSearchParams(location.search).get('return'));
    if (!ret || !ret.startsWith('/api/oidc/authorize')) {
        throw new Error(`authorize did not bounce to /?return=<authorize>; got return=${ret}`);
    }
    console.log('authorize bounce → /?return=<authorize>: OK');

    // 2. Log in with TOTP on the bounced-to SPA.
    if (!(await findText(page, 'Sign in'))) throw new Error('no Sign in screen after bounce');
    await page.keyboard.type('admin');
    await page.keyboard.press('Enter');
    await sleep(800);
    await page.keyboard.type(totp(SECRET));
    await page.keyboard.press('Enter');

    // 3+4. enterMain() should fire consumeOidcReturn() and replace() back to
    //      authorize, which now issues a code and 302s to the redirect_uri.
    //      Poll for the landing on the app callback.
    let landed = null;
    for (let i = 0; i < 40; i++) {
        await sleep(300);
        const u = page.url();
        if (u.startsWith(REDIRECT + '?')) { landed = u; break; }
    }
    if (!landed) {
        const diag = await page.evaluate(() => ({
            url: location.href,
            texts: [...document.querySelectorAll('div.text')].map(e => e.textContent.trim()).filter(Boolean).slice(0, 20),
            hasFn: typeof consumeOidcReturn,
        })).catch(e => ({ err: String(e) }));
        console.error('DIAG:', JSON.stringify(diag, null, 2));
        throw new Error(`did not land on redirect_uri after login; still at ${page.url()}`);
    }
    const cb = new URL(landed);
    const code = cb.searchParams.get('code');
    const gotState = cb.searchParams.get('state');
    if (!code) throw new Error(`callback carried no ?code= : ${landed}`);
    if (gotState !== STATE) throw new Error(`state mismatch: sent ${STATE}, got ${gotState}`);
    console.log(`handoff → ${cb.pathname}?code=${code.slice(0, 8)}…&state=${gotState}: OK`);

    // 5. Open-redirect guard, live: now that we hold a session, loading the
    //    SPA with a malicious ?return must NOT navigate away — enterMain()
    //    drops the value and shows the dashboard instead.
    const evil = '//evil.example/phish';
    await page.goto(BASE + '/?return=' + encodeURIComponent(evil), { waitUntil: 'networkidle0' });
    await sleep(1500);
    const host = await page.evaluate(() => location.host);
    if (host !== 'localhost:8443') throw new Error(`open-redirect guard FAILED: navigated to ${host}`);
    if (!(await findText(page, 'Dashboard'))) throw new Error('no dashboard after blocked open-redirect');
    console.log('open-redirect guard (//evil rejected, stayed on dashboard): OK');

    console.log('ALL OK');
} catch (err) {
    failed = true;
    console.error('FAIL:', err.message);
    try { await page.screenshot({ path: '/tmp/flynas-uitest/oidc-fail.png' }); } catch (e) {}
}
await browser.close();
process.exit(failed ? 1 : 0);
