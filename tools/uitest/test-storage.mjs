// FlyNAS Storage page UI test: verify volume display, two-click
// delete, disk selection + volume create, scrub, auto-scrub toggle.
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

// Click the row element to the left of a text label (e.g. a disk's
// checkbox cell): just click slightly left of the label.
async function clickRowOf(page, text) {
    const pos = await findText(page, text);
    if (!pos) throw new Error(`row not found: ${text}`);
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
    await page.keyboard.type('admin');
    await page.keyboard.press('Enter');
    await sleep(800);
    await page.keyboard.type(totp(SECRET));
    await page.keyboard.press('Enter');
    await sleep(1200);
    console.log('login: OK');

    await clickText(page, 'Storage');
    await sleep(1500);
    await page.screenshot({ path: '/tmp/flynas-uitest/storage-1.png' });

    for (const t of ['Volumes', 'Disks', 'tank']) {
        if (!(await findText(page, t))) throw new Error(`storage page missing: ${t}`);
    }
    if (!(await findText(page, 'vbd1, vbd2'))) throw new Error('volume disk list missing');
    if (!(await findText(page, 'system'))) throw new Error('system disk tag missing');
    console.log('storage page renders volumes + disks: OK');

    // Scrub now
    await clickText(page, 'Scrub now');
    await sleep(2500);

    // Auto-scrub toggle: flip whatever the current state is
    const wasOn = !!(await findText(page, 'Auto-scrub on'));
    await clickText(page, wasOn ? 'Auto-scrub on' : 'Auto-scrub off');
    await sleep(1500);
    if (!(await findText(page, wasOn ? 'Auto-scrub off' : 'Auto-scrub on')))
        throw new Error('auto-scrub did not toggle');
    console.log('scrub + auto-scrub toggle: OK');

    // Create a volume on vbd3 + vbd4: click their checkbox rows
    await clickRowOf(page, 'vbd3');
    await sleep(300);
    await clickRowOf(page, 'vbd4');
    await sleep(300);
    const checked = await page.evaluate(() =>
        [...document.querySelectorAll('div.text')].filter(e => e.textContent.trim() === '[x]').length);
    if (checked !== 2) throw new Error(`expected 2 selected disks, got ${checked}`);
    const input = await findText(page, 'volume name');
    await page.mouse.click(input.x, input.y, { delay: 120 });
    await sleep(200);
    await page.keyboard.type('pool');
    await clickText(page, 'Create');
    await sleep(6000); // newfs + mount takes a few seconds
    if (!(await findText(page, 'pool'))) throw new Error('pool volume missing after create');
    if (!(await findText(page, 'vbd3, vbd4'))) throw new Error('pool disk list wrong');
    console.log('create volume via UI: OK');

    // Two-click delete on tank
    const del = await page.evaluate(() => {
        // first Delete button belongs to first volume (tank)
        const els = [...document.querySelectorAll('div.text')].filter(e => e.textContent.trim() === 'Delete');
        if (!els.length) return null;
        const r = els[0].getBoundingClientRect();
        return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    });
    if (!del) throw new Error('Delete button not found');
    await page.mouse.click(del.x, del.y, { delay: 120 });
    await sleep(500);
    if (!(await findText(page, 'Confirm?'))) throw new Error('delete did not arm');
    await clickText(page, 'Confirm?');
    await sleep(2500);
    if (await findText(page, 'vbd1, vbd2')) throw new Error('tank still present after delete');
    console.log('two-click volume delete: OK');

    await page.screenshot({ path: '/tmp/flynas-uitest/storage-2.png' });
    console.log('ALL OK');
} catch (err) {
    failed = true;
    console.error('FAIL:', err.message);
    await page.screenshot({ path: '/tmp/flynas-uitest/fail.png' });
}
await browser.close();
process.exit(failed ? 1 : 0);
