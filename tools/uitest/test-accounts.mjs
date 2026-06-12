// FlyNAS Accounts page UI test: login with TOTP, navigate to
// Accounts, add a user via the Clay input, toggle group membership.
import puppeteer from 'puppeteer-core';
import { createHmac } from 'crypto';

const SECRET = process.env.TOTP_SECRET;
const BASE = 'https://localhost:8443';

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

// Clay renders text as positioned divs; find one by content and
// return its center for clicking.
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
    const pos = await findText(page, text);
    if (!pos) throw new Error(`text not found: ${text}`);
    await page.mouse.click(pos.x, pos.y, { delay: 120 });
}

const browser = await puppeteer.launch({
    executablePath: '/usr/bin/google-chrome',
    headless: 'new',
    args: ['--no-sandbox', '--ignore-certificate-errors', '--window-size=1280,800'],
    defaultViewport: { width: 1280, height: 800 },
});
const page = await browser.newPage();
let failed = false;
try {
    await page.goto(BASE, { waitUntil: 'networkidle0' });
    await sleep(500);

    // Login screen → username
    if (!(await findText(page, 'Sign in'))) throw new Error('no Sign in screen');
    await page.keyboard.type('admin');
    await page.keyboard.press('Enter');
    await sleep(800);

    // TOTP code
    await page.keyboard.type(totp(SECRET));
    await page.keyboard.press('Enter');
    await sleep(1200);

    if (!(await findText(page, 'Dashboard'))) throw new Error('no dashboard after login');
    console.log('login: OK');

    // Navigate to Accounts
    await clickText(page, 'Accounts');
    await sleep(800);
    await page.screenshot({ path: '/tmp/flynas-uitest/accounts-1.png' });

    for (const t of ['Users', 'Groups', 'admin', 'testuser1', 'testgrp']) {
        if (!(await findText(page, t))) throw new Error(`accounts page missing: ${t}`);
    }
    console.log('accounts page renders users + groups: OK');

    // Add a user through the Clay input
    const input = await findText(page, 'new username');
    await page.mouse.click(input.x, input.y, { delay: 120 });
    await sleep(200);
    await page.keyboard.type('uitestuser');
    await sleep(200);
    await clickText(page, 'Add User');
    await sleep(1200);
    if (!(await findText(page, 'uitestuser'))) throw new Error('uitestuser row missing after add');
    console.log('add user via UI: OK');

    // Open group membership and toggle uitestuser into testgrp
    await clickText(page, 'testgrp');
    await sleep(800);
    if (!(await findText(page, '[ ]'))) throw new Error('membership checkboxes missing');
    // click the checkbox row next to uitestuser: find its [ ] sibling by y proximity
    const target = await page.evaluate(() => {
        const rows = [...document.querySelectorAll('div.text')];
        const boxes = rows.filter(b => ['[ ]', '[x]'].includes(b.textContent.trim()));
        for (const b of boxes) {
            const br = b.getBoundingClientRect();
            const u = rows.find(el => el.textContent.trim() === 'uitestuser' &&
                Math.abs(el.getBoundingClientRect().y - br.y) < 3 &&
                el.getBoundingClientRect().x > br.x);
            if (u) return { x: br.x + br.width / 2, y: br.y + br.height / 2 };
        }
        return null;
    });
    if (!target) throw new Error('uitestuser membership row not found');
    await page.mouse.click(target.x, target.y, { delay: 120 });
    await sleep(1200);
    if (!(await findText(page, '1 member'))) throw new Error('member count did not update');
    console.log('group membership toggle: OK');

    // SSH toggle on uitestuser
    const sshBefore = await page.evaluate(() =>
        [...document.querySelectorAll('div.text')].filter(e => e.textContent.trim() === 'SSH on').length);
    // click the SSH button on uitestuser's row
    const ssh = await page.evaluate(() => {
        const rows = [...document.querySelectorAll('div.text')];
        const user = rows.find(el => el.textContent.trim() === 'uitestuser');
        if (!user) return null;
        const uy = user.getBoundingClientRect().y;
        const btn = rows.find(b => b.textContent.trim().startsWith('SSH') &&
            Math.abs(b.getBoundingClientRect().y - uy) < 8);
        if (!btn) return null;
        const r = btn.getBoundingClientRect();
        return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    });
    if (!ssh) throw new Error('SSH button not found');
    await page.mouse.click(ssh.x, ssh.y, { delay: 120 });
    await sleep(1200);
    const sshAfter = await page.evaluate(() =>
        [...document.querySelectorAll('div.text')].filter(e => e.textContent.trim() === 'SSH on').length);
    if (sshAfter !== sshBefore + 1) throw new Error(`SSH toggle: before=${sshBefore} after=${sshAfter}`);
    console.log('SSH toggle: OK');

    await page.screenshot({ path: '/tmp/flynas-uitest/accounts-2.png' });
    console.log('ALL OK');
} catch (err) {
    failed = true;
    console.error('FAIL:', err.message);
    await page.screenshot({ path: '/tmp/flynas-uitest/fail.png' });
}
await browser.close();
process.exit(failed ? 1 : 0);
