// FlyNAS Apps page UI test: the catalog renders, one-click install of
// Seafile creates a VM (visible on the VMs page), then clean up by
// deleting that VM.
//
// Preconditions: none. Creates VM "seafiletest"; cleanest with an
// empty vms table and no orphan /usr/local/flynas/vms/seafiletest.img.
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

async function findText(page, text, { prefix = false } = {}) {
    return page.evaluate((t, pre) => {
        for (const el of document.querySelectorAll('div.text')) {
            const c = el.textContent.trim();
            if (pre ? c.startsWith(t) : c === t) {
                const r = el.getBoundingClientRect();
                return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
            }
        }
        return null;
    }, text, prefix);
}

async function clickText(page, text, opts) {
    const pos = await findText(page, text, opts);
    if (!pos) throw new Error(`text not found: ${text}`);
    await page.mouse.click(pos.x, pos.y, { delay: 120 });
}

async function clickNthText(page, text, nth) {
    const pos = await page.evaluate((t, n) => {
        const els = [...document.querySelectorAll('div.text')]
            .filter(e => e.textContent.trim() === t);
        if (n < 0) n += els.length;
        if (!els[n]) return null;
        const r = els[n].getBoundingClientRect();
        return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    }, text, nth);
    if (!pos) throw new Error(`text #${nth} not found: ${text}`);
    await page.mouse.click(pos.x, pos.y, { delay: 120 });
}

async function waitFor(page, text, opts, tries = 12) {
    for (let i = 0; i < tries; i++) {
        if (await findText(page, text, opts)) return true;
        await sleep(1000);
    }
    return false;
}

const browser = await puppeteer.launch({
    executablePath: '/usr/bin/google-chrome',
    headless: 'new',
    args: ['--no-sandbox', '--ignore-certificate-errors', '--window-size=1280,900'],
    defaultViewport: { width: 1280, height: 900 },
});
const page = await browser.newPage();
let failed = false;
try {
    await page.goto(BASE, { waitUntil: 'networkidle0' });
    await sleep(500);
    await page.keyboard.type('admin');
    await page.keyboard.press('Enter');
    await sleep(800);
    await page.keyboard.type(totp(SECRET));
    await page.keyboard.press('Enter');
    await sleep(1200);
    console.log('login: OK');

    await clickText(page, 'Apps');
    if (!(await waitFor(page, 'App catalog'))) throw new Error('apps page did not load');
    for (const t of ['Seafile', 'Jellyfin', 'Forgejo']) {
        if (!(await findText(page, t))) throw new Error('catalog missing: ' + t);
    }
    await page.screenshot({ path: '/tmp/flynas-uitest/apps-1.png' });
    console.log('catalog renders: OK');
    await sleep(1000);

    // Fill the shared install target, then install Seafile
    await clickText(page, 'VM name');
    await page.keyboard.type('seafiletest');
    await clickText(page, 'static IP (optional)');
    await page.keyboard.type('192.168.50.30');
    // Catalog is alphabetical (CryptPad, DokuWiki, Forgejo, Jellyfin,
    // Readeck, RoundCube, Seafile, VaultWarden, Wekan) so Seafile's
    // Install is the 7th (index 6). Retry — the click can be eaten.
    let installed = false;
    for (let attempt = 0; attempt < 3 && !installed; attempt++) {
        await clickNthText(page, 'Install', 6);
        await sleep(1500);
        installed = await waitFor(page, 'Seafile installed', { prefix: true }, 6);
    }
    if (!installed) throw new Error('Seafile install produced no confirmation');
    await page.screenshot({ path: '/tmp/flynas-uitest/apps-2.png' });
    console.log('one-click install: OK');

    // Verify the VM landed on the VMs page
    await clickText(page, 'VMs');
    if (!(await waitFor(page, 'seafiletest'))) throw new Error('installed VM not on VMs page');
    console.log('installed VM appears on VMs page: OK');

    // Clean up: delete the VM (two-click)
    await sleep(800);
    await clickText(page, 'Delete');
    await sleep(500);
    if (!(await findText(page, 'Confirm?'))) throw new Error('delete did not arm');
    await clickText(page, 'Confirm?');
    let gone = false;
    for (let i = 0; i < 10 && !gone; i++) {
        await sleep(1000);
        gone = !(await findText(page, 'seafiletest'));
    }
    if (!gone) throw new Error('VM still present after delete');
    console.log('cleanup delete: OK');
    console.log('ALL OK');
} catch (err) {
    failed = true;
    console.error('FAIL:', err.message);
    await page.screenshot({ path: '/tmp/flynas-uitest/fail.png' });
}
await browser.close();
process.exit(failed ? 1 : 0);
