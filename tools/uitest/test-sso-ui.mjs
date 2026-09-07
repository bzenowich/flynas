// FlyNAS App-access (SSO) UI test. Drives the grant matrix on the Apps
// page and checks the DB as ground truth:
//   1. login (admin) → Apps page → "App access (SSO)" card shows the client
//   2. click the client → grant matrix lists users, target ungranted
//   3. tick the target's box → app_grants row appears (role 'user')
//   4. click the role toggle → role flips to 'admin'
//   5. untick → grant removed
//
// Seed/cleanup (client `sso-ui-client` + user `ssouitestuser`) is done by
// run-sso-ui.sh over ssh; run this test through that script (or run-all.sh),
// which also exports FLYNAS_SSH for the DB checks below.
import puppeteer from 'puppeteer-core';
import { createHmac } from 'crypto';
import { execSync } from 'child_process';

const SECRET = process.env.TOTP_SECRET;
const BASE = 'https://localhost:8443';
const TARGET = 'ssouitestuser';

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

// Ground-truth: app_grants rows for the target user, via ssh sqlite.
function dbRole() {
    const sql = "SELECT ag.role FROM app_grants ag JOIN users u ON u.id=ag.user_id " +
        `WHERE u.username='${TARGET}';`;
    // Pipe to `sh` on the VM over its stdin — the remote login shell is tcsh
    // and re-parses inline args, breaking on the SQL's spaces/quotes.
    //
    // FLYNAS_SSH is the whole ssh invocation (port, key, known_hosts), exported
    // by ../../bin/_common.sh through the run-*.sh wrapper. Running this test
    // by hand without it needs an ssh-config alias, which the claude-box
    // sandbox does not have — use run-sso-ui.sh or run-all.sh.
    const ssh = process.env.FLYNAS_SSH || 'ssh h2dev';
    return execSync(`${ssh} sh`,
        { encoding: 'utf8', input: `sqlite3 /usr/local/flynas/flynas.db "${sql}"\n` }).trim();
}

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
async function clickText(page, text) {
    const p = await findText(page, text);
    if (!p) throw new Error(`text not found: ${text}`);
    await page.mouse.click(p.x, p.y, { delay: 120 });
}
// Click a control (by exact text) on the row horizontally aligned with `name`.
async function clickRowControl(page, name, controls) {
    const pos = await page.evaluate((nm, ctrls) => {
        const rows = [...document.querySelectorAll('div.text')];
        const u = rows.find(e => e.textContent.trim() === nm);
        if (!u) return null;
        const uy = u.getBoundingClientRect().y;
        const btn = rows.find(b => ctrls.includes(b.textContent.trim()) &&
            Math.abs(b.getBoundingClientRect().y - uy) < 6);
        if (!btn) return null;
        const r = btn.getBoundingClientRect();
        return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    }, name, controls);
    if (!pos) throw new Error(`control ${controls} not found on row ${name}`);
    await page.mouse.click(pos.x, pos.y, { delay: 120 });
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
    await page.goto(BASE, { waitUntil: 'networkidle0' });
    await sleep(500);
    await page.keyboard.type('admin'); await page.keyboard.press('Enter'); await sleep(800);
    await page.keyboard.type(totp(SECRET)); await page.keyboard.press('Enter'); await sleep(1200);
    if (!(await findText(page, 'Dashboard'))) throw new Error('login failed');
    console.log('login: OK');

    await clickText(page, 'Apps');
    await sleep(900);
    if (!(await findText(page, 'App access (SSO)'))) throw new Error('SSO card missing');
    if (!(await findText(page, 'SSO UI Test'))) throw new Error('seeded client not listed');
    console.log('SSO card + client listed: OK');

    // Open the client → grant matrix should list the target user.
    await clickText(page, 'SSO UI Test');
    await sleep(900);
    if (!(await findText(page, TARGET))) throw new Error('target user not in matrix');
    if (dbRole() !== '') throw new Error('precondition: target already granted');
    console.log('matrix opened, target ungranted: OK');

    // Grant: tick the target's checkbox.
    await clickRowControl(page, TARGET, ['[ ]']);
    await sleep(1000);
    if (dbRole() !== 'user') throw new Error(`grant not created in DB; role=${JSON.stringify(dbRole())}`);
    if (!(await findText(page, 'user'))) throw new Error('role button did not render after grant');
    console.log('grant created (DB role=user): OK');

    // Role toggle: user → admin.
    await clickRowControl(page, TARGET, ['user']);
    await sleep(1000);
    if (dbRole() !== 'admin') throw new Error(`role did not flip in DB; role=${JSON.stringify(dbRole())}`);
    console.log('role toggle (DB role=admin): OK');

    // Revoke: untick.
    await clickRowControl(page, TARGET, ['[x]']);
    await sleep(1000);
    if (dbRole() !== '') throw new Error(`grant not revoked; role=${JSON.stringify(dbRole())}`);
    console.log('grant revoked (DB empty): OK');

    console.log('ALL OK');
} catch (err) {
    failed = true;
    console.error('FAIL:', err.message);
    try { await page.screenshot({ path: '/tmp/flynas-uitest/sso-ui-fail.png' }); } catch (e) {}
}
await browser.close();
process.exit(failed ? 1 : 0);
