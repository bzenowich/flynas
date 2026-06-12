// FlyNAS Backup page UI test: snapshot create + two-click delete,
// auto-snapshot toggle, S3 bucket add/delete, backup-now + status
// line, restore browser navigation.
//
// Preconditions (seeded by the runner): volume "tank" with files
// /data/tank/docs/readme.txt + /data/tank/big.bin, S3 bucket
// "testbucket" (local endpoint) that has completed at least one sync.
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

async function countText(page, text, { prefix = false } = {}) {
    return page.evaluate((t, pre) => {
        let n = 0;
        for (const el of document.querySelectorAll('div.text')) {
            const c = el.textContent.trim();
            if (pre ? c.startsWith(t) : c === t) n++;
        }
        return n;
    }, text, prefix);
}

async function clickText(page, text, opts) {
    const pos = await findText(page, text, opts);
    if (!pos) throw new Error(`text not found: ${text}`);
    await page.mouse.click(pos.x, pos.y, { delay: 120 });
}

// nth occurrence (0-based) of an exact text, e.g. the right Delete button
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

    await clickText(page, 'Backup');
    let loaded = false;
    for (let i = 0; i < 10 && !loaded; i++) {
        await sleep(1000);
        loaded = !!(await findText(page, 'tank'));
    }
    if (!loaded) throw new Error('backup page did not load');
    await page.screenshot({ path: '/tmp/flynas-uitest/backup-1.png' });

    for (const t of ['Snapshots', 'Offsite backup (S3)', 'testbucket', 'Snapshot now']) {
        if (!(await findText(page, t))) throw new Error(`backup page missing: ${t}`);
    }
    if (!(await findText(page, 'snap-', { prefix: true })))
        throw new Error('no snapshot rows rendered');
    console.log('backup page renders snapshots + buckets: OK');

    // Manual snapshot
    const manualBefore = await countText(page, 'snap-m-', { prefix: true });
    await clickText(page, 'Snapshot now');
    let snapped = false;
    for (let i = 0; i < 8 && !snapped; i++) {
        await sleep(1000);
        snapped = (await countText(page, 'snap-m-', { prefix: true })) === manualBefore + 1;
    }
    if (!snapped) throw new Error('manual snapshot did not appear');
    console.log('manual snapshot via UI: OK');

    // Two-click delete of the newest snapshot (first row in the card)
    const snapsBefore = await countText(page, 'snap-', { prefix: true });
    await clickNthText(page, 'Delete', 0);
    await sleep(500);
    if (!(await findText(page, 'Confirm?'))) throw new Error('snapshot delete did not arm');
    await clickText(page, 'Confirm?');
    let deleted = false;
    for (let i = 0; i < 8 && !deleted; i++) {
        await sleep(1000);
        deleted = (await countText(page, 'snap-', { prefix: true })) === snapsBefore - 1;
    }
    if (!deleted) throw new Error('snapshot did not disappear after delete');
    console.log('two-click snapshot delete: OK');

    // Auto-snapshot toggle round-trip
    const wasOn = !!(await findText(page, 'Auto-snapshot on'));
    await clickText(page, wasOn ? 'Auto-snapshot on' : 'Auto-snapshot off');
    await sleep(1500);
    if (!(await findText(page, wasOn ? 'Auto-snapshot off' : 'Auto-snapshot on')))
        throw new Error('auto-snapshot did not toggle');
    await clickText(page, wasOn ? 'Auto-snapshot off' : 'Auto-snapshot on');
    await sleep(1500);
    if (!(await findText(page, wasOn ? 'Auto-snapshot on' : 'Auto-snapshot off')))
        throw new Error('auto-snapshot did not toggle back');
    console.log('auto-snapshot toggle: OK');

    // Backup now: status line shows running, then completed
    await clickText(page, 'Backup now');
    let done = false;
    for (let i = 0; i < 30 && !done; i++) {
        await sleep(1000);
        done = !!(await findText(page, 'Last backup completed', { prefix: true }));
    }
    if (!done) throw new Error('backup did not complete');
    console.log('backup now + status line: OK');

    // Restore browser: volumes pseudo-root -> tank -> docs -> back up
    await clickText(page, 'Browse');
    await sleep(800);
    if (!(await findText(page, 'Restore browser'))) throw new Error('browse card missing');
    await clickText(page, 'tank/');
    await sleep(1500);
    if (!(await findText(page, 'docs/', { prefix: true }))) throw new Error('docs/ missing in browse');
    if (!(await findText(page, 'big.bin', { prefix: true }))) throw new Error('big.bin missing in browse');
    await clickText(page, 'docs/', { prefix: true });
    await sleep(1500);
    if (!(await findText(page, 'readme.txt', { prefix: true }))) throw new Error('readme.txt missing');
    await clickText(page, '../');
    await sleep(1500);
    if (!(await findText(page, 'big.bin', { prefix: true }))) throw new Error('did not navigate back up');
    await page.screenshot({ path: '/tmp/flynas-uitest/backup-2.png' });
    console.log('restore browser navigation: OK');

    // Add + delete a second bucket
    await clickText(page, 'my-backups');         // bucket name input
    await page.keyboard.type('bucket2');
    await clickText(page, 's3.amazonaws.com or /local/dir');
    await page.keyboard.type('/var/flynas-s3test2');
    await clickText(page, 'encryption password (min 8)');
    await page.keyboard.type('testpass456');
    await clickText(page, 'Add bucket');
    let added = false;
    for (let i = 0; i < 8 && !added; i++) {
        await sleep(1000);
        added = !!(await findText(page, 'bucket2'));
    }
    if (!added) throw new Error('bucket2 missing after add');
    await clickNthText(page, 'Delete', -1);      // last Delete = bucket2's
    await sleep(500);
    await clickText(page, 'Confirm?');
    let removed = false;
    for (let i = 0; i < 8 && !removed; i++) {
        await sleep(1000);
        removed = !(await findText(page, 'bucket2'));
    }
    if (!removed) throw new Error('bucket2 still present after delete');
    console.log('S3 bucket add/delete: OK');

    await page.screenshot({ path: '/tmp/flynas-uitest/backup-3.png' });
    console.log('ALL OK');
} catch (err) {
    failed = true;
    console.error('FAIL:', err.message);
    await page.screenshot({ path: '/tmp/flynas-uitest/fail.png' });
}
await browser.close();
process.exit(failed ? 1 : 0);
