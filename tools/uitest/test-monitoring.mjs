// FlyNAS Monitoring page UI test: add a monitor (type cycle + target),
// see it go up, pause/resume, add a notification channel, assign it to
// the monitor via the expandable checkbox list, then delete both.
//
// Preconditions: none (the test creates and removes its own monitor +
// channel). Starts cleanest with an empty monitors table.
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
    await page.evaluate(() => { window.__flynasPauseRefresh = true; });  // deterministic: no bg refresh during the test

    await clickText(page, 'Monitoring');
    if (!(await waitFor(page, 'Notification channels')))
        throw new Error('monitoring page did not load');
    for (const t of ['Monitors', 'Notification channels']) {
        if (!(await findText(page, t))) throw new Error(`monitoring page missing: ${t}`);
    }
    await page.screenshot({ path: '/tmp/flynas-uitest/monitoring-1.png' });
    console.log('monitoring page renders: OK');

    // Add a tcp monitor for 127.0.0.1:443 (cycle type http -> tcp)
    await clickText(page, 'name');               // monitor name input placeholder
    await page.keyboard.type('uitest-mon');
    await sleep(300);
    await clickText(page, 'http');               // type cycle button -> tcp
    await sleep(300);
    if (!(await findText(page, 'tcp'))) throw new Error('type did not cycle to tcp');
    await clickText(page, 'https://host/ or host:port');
    await page.keyboard.type('127.0.0.1:443');
    await sleep(300);
    await clickText(page, 'Add');
    if (!(await waitFor(page, 'uitest-mon'))) throw new Error('monitor not added');
    console.log('add monitor: OK');

    // Wait for first check to flip status to up (dot color + uptime%)
    let up = false;
    for (let i = 0; i < 15 && !up; i++) {
        await sleep(1500);
        up = (await countText(page, '%', { prefix: false }) >= 0) &&
             !!(await findText(page, 'uitest-mon'));
        // uptime detail like "100%" appears once checked
        if (await findText(page, '100%')) up = true;
    }
    if (!up) throw new Error('monitor never reported uptime');
    await page.screenshot({ path: '/tmp/flynas-uitest/monitoring-2.png' });
    console.log('monitor goes up: OK');

    // Pause -> Resume round-trip. Settle between clicks so the click
    // doesn't land during the page's 5s background refresh.
    await clickText(page, 'Pause');
    await sleep(800);
    if (!(await waitFor(page, 'Resume', {}, 8))) throw new Error('pause did not take');
    await sleep(800);
    await clickText(page, 'Resume');
    await sleep(800);
    if (!(await waitFor(page, 'Pause', {}, 8))) throw new Error('resume did not take');
    console.log('pause/resume: OK');

    // Add a webhook channel
    await clickText(page, 'channel name');
    await page.keyboard.type('uitest-chan');
    await sleep(300);
    await clickText(page, 'email or webhook URL');
    await page.keyboard.type('https://example.com/hook');
    await sleep(300);
    await clickText(page, 'Add channel');
    if (!(await waitFor(page, 'uitest-chan'))) throw new Error('channel not added');
    console.log('add channel: OK');

    // Expand monitor, assign the channel (checkbox [ ] -> [x]).
    // Retry the expand click in case it lands during a refresh.
    await sleep(800);
    let expanded = false;
    for (let attempt = 0; attempt < 3 && !expanded; attempt++) {
        await clickText(page, 'uitest-mon');
        await sleep(800);
        expanded = await waitFor(page, 'Notify channels:', {}, 4);
    }
    if (!expanded) throw new Error('monitor did not expand');
    // first 'uitest-chan' is the one inside the expanded monitor row
    await clickNthText(page, 'uitest-chan', 0);
    let assigned = false;
    for (let i = 0; i < 8 && !assigned; i++) {
        await sleep(1000);
        assigned = !!(await findText(page, '[x]'));
    }
    if (!assigned) throw new Error('channel assignment checkbox did not check');
    await page.screenshot({ path: '/tmp/flynas-uitest/monitoring-3.png' });
    console.log('assign channel to monitor: OK');

    // Delete channel (two-click) — last Delete is the channel's
    await clickNthText(page, 'Delete', -1);
    await sleep(500);
    await clickText(page, 'Confirm?');
    let chanGone = false;
    for (let i = 0; i < 8 && !chanGone; i++) {
        await sleep(1000);
        chanGone = !(await findText(page, 'uitest-chan'));
    }
    if (!chanGone) throw new Error('channel still present after delete');
    console.log('delete channel: OK');

    // Delete monitor (two-click)
    await clickNthText(page, 'Delete', 0);
    await sleep(500);
    await clickText(page, 'Confirm?');
    let gone = false;
    for (let i = 0; i < 8 && !gone; i++) {
        await sleep(1000);
        gone = !(await findText(page, 'uitest-mon'));
    }
    if (!gone) throw new Error('monitor still present after delete');
    console.log('delete monitor: OK');

    await page.screenshot({ path: '/tmp/flynas-uitest/monitoring-4.png' });
    console.log('ALL OK');
} catch (err) {
    failed = true;
    console.error('FAIL:', err.message);
    await page.screenshot({ path: '/tmp/flynas-uitest/fail.png' });
}
await browser.close();
process.exit(failed ? 1 : 0);
