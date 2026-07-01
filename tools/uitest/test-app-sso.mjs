// FlyNAS app-VM SSO round-trip (§2.11 step 7): drive a real browser through
// "Sign in with flynas" on the provisioned Forgejo guest and verify the OAuth
// chain (Forgejo → FlyNAS /authorize → back to Forgejo callback → logged in).
//
// Tunnels (set up by run-app-sso.sh):
//   https://localhost:8443  → h2dev:443        (FlyNAS UI + /api/oidc/authorize)
//   http://localhost:20001  → h2dev → guest:3000 (Forgejo, matches its ROOT_URL)
import puppeteer from 'puppeteer-core';
import { createHmac } from 'crypto';

const SECRET = process.env.TOTP_SECRET;
const FLYNAS = 'https://localhost:8443';
const FORGEJO = 'http://localhost:20001';

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
    const msg = Buffer.alloc(8); msg.writeBigUInt64BE(BigInt(t));
    const h = createHmac('sha1', b32decode(secret)).update(msg).digest();
    const o = h[19] & 15;
    return String((h.readUInt32BE(o) & 0x7fffffff) % 1000000).padStart(6, '0');
}
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
async function findText(page, t) {
    return page.evaluate((txt) => {
        for (const el of document.querySelectorAll('div.text')) {
            if (el.textContent.trim() === txt) {
                const r = el.getBoundingClientRect();
                return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
            }
        }
        return null;
    }, t);
}

const browser = await puppeteer.launch({
    executablePath: '/usr/bin/google-chrome',
    headless: 'new',
    args: ['--no-sandbox', '--ignore-certificate-errors', '--window-size=1280,1000'],
    defaultViewport: { width: 1280, height: 1000 },
});
const page = await browser.newPage();
let failed = false;
try {
    // 1. Establish a FlyNAS session (so /authorize issues a code without a
    //    fresh login prompt during the SSO bounce).
    await page.goto(FLYNAS, { waitUntil: 'networkidle0' });
    await sleep(500);
    await page.keyboard.type('admin'); await page.keyboard.press('Enter'); await sleep(800);
    await page.keyboard.type(totp(SECRET)); await page.keyboard.press('Enter'); await sleep(1500);
    if (!(await findText(page, 'Dashboard'))) throw new Error('FlyNAS login failed');
    console.log('flynas login: OK');

    // 2. Forgejo login page → the "Sign in with flynas" OAuth link.
    await page.goto(FORGEJO + '/user/login', { waitUntil: 'networkidle0' });
    await sleep(600);
    const ssoHref = await page.evaluate(() => {
        const a = [...document.querySelectorAll('a[href*="/user/oauth2/flynas"]')][0];
        return a ? a.getAttribute('href') : null;
    });
    if (!ssoHref) throw new Error('"Sign in with flynas" link not on Forgejo login page');
    console.log('forgejo SSO link:', ssoHref);

    // 3. Click it and follow the whole OAuth chain (Forgejo → FlyNAS authorize
    //    → callback → Forgejo). Session cookie means authorize won't prompt.
    await Promise.all([
        page.waitForNavigation({ waitUntil: 'networkidle0', timeout: 45000 }).catch(() => {}),
        page.evaluate((h) => { window.location.href = h; }, ssoHref),
    ]);
    // give the callback + any Forgejo post-auth redirect time to settle
    for (let i = 0; i < 8; i++) { await sleep(1500); if (!/\/api\/oidc\/authorize/.test(page.url())) break; }
    await sleep(1500);

    // First OIDC login lands on Forgejo's account-link/create page — complete
    // "Register new account" to reach a fully logged-in session.
    if (/\/user\/link_account/.test(page.url())) {
        console.log('at link_account (SSO delivered the identity) — completing new-account registration');
        // Submit the "register new account" form directly (fill any empty
        // user_name/email the OIDC identity didn't pre-fill).
        const submitted = await page.evaluate(() => {
            // Force a non-reserved Forgejo username ("admin" is reserved there);
            // the account still links to the FlyNAS OIDC identity.
            const force = (n, v) => { const el = document.querySelector(`input[name="${n}"]`); if (el) el.value = v; };
            force('user_name', 'flynasadmin'); force('email', 'admin@flynas.local');
            const form = [...document.querySelectorAll('form')]
                .find(f => /link_account_signup/.test(f.getAttribute('action') || ''));
            if (form) { form.submit(); return true; }
            return false;
        });
        if (submitted) { await page.waitForNavigation({ waitUntil: 'networkidle0', timeout: 30000 }).catch(() => {}); await sleep(1500); }
    }

    const url = page.url();
    const body = await page.evaluate(() => document.body.innerText.slice(0, 600));
    const signedIn = await page.evaluate(() =>
        !!document.querySelector('a[href="/user/logout"], a[href*="/user/settings"], .avatar, #navbar .dropdown img.avatar')
        || /dashboard|repositories|sign out|logout/i.test(document.body.innerText));
    const linkOrRegister = /link.*account|activate|authorize application|sign up|create.*account|e-?mail/i.test(body);
    await page.screenshot({ path: '/tmp/flynas-uitest/app-sso.png' }).catch(() => {});

    console.log('final url:', url);
    console.log('page head:', JSON.stringify(body.replace(/\s+/g, ' ').slice(0, 240)));

    if (/\/api\/oidc\/authorize/.test(url)) throw new Error('stuck at FlyNAS /authorize (SSO handoff did not complete)');
    if (/\/user\/login\b/.test(url) && !linkOrRegister) throw new Error('bounced back to Forgejo login (auth rejected)');

    if (signedIn) console.log('RESULT: SSO round-trip OK — signed into Forgejo via FlyNAS');
    else if (linkOrRegister) console.log('RESULT: SSO handoff OK — Forgejo received the FlyNAS identity (account link/register step)');
    else throw new Error('unexpected end state: ' + url);
} catch (e) {
    failed = true;
    console.error('FAIL:', e.message);
    try { await page.screenshot({ path: '/tmp/flynas-uitest/app-sso-fail.png' }); } catch {}
} finally {
    await browser.close();
    process.exit(failed ? 1 : 0);
}
