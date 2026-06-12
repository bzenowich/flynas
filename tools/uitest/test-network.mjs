// FlyNAS Network page UI test: verify IP config display, DHCP/static
// mode toggle with input fields, two-click apply arming, timezone
// save, NTP save + disable.
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

// Click an input box by its current text or placeholder
async function focusInput(page, text) {
    await clickText(page, text);
    await sleep(200);
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

    await clickText(page, 'Network');
    await sleep(1500);
    await page.screenshot({ path: '/tmp/flynas-uitest/network-1.png' });

    for (const t of ['IP Configuration', 'Time', 'vtnet0', 'DHCP',
                     '52:54:00:12:34:56', 'UTC']) {
        if (!(await findText(page, t))) throw new Error(`network page missing: ${t}`);
    }
    if (!(await findText(page, 'NTP ', { prefix: true })))
        throw new Error('NTP state text missing');
    if (!(await findText(page, '10.0.2.15 / 255.255.255.0 via 10.0.2.2')))
        throw new Error('live address line missing');
    console.log('network page renders config: OK');

    // Mode toggle reveals static inputs (pre-filled from live config)
    await clickText(page, 'DHCP');
    await sleep(500);
    for (const t of ['Static', 'IP address', 'Netmask', 'Gateway']) {
        if (!(await findText(page, t))) throw new Error(`static mode missing: ${t}`);
    }
    if (!(await findText(page, '10.0.2.15'))) throw new Error('ip input not pre-filled');
    console.log('static mode inputs: OK');

    // Apply is two-click: first click arms
    await clickText(page, 'Apply');
    await sleep(500);
    if (!(await findText(page, 'Restart network?'))) throw new Error('apply did not arm');
    // Toggling back to DHCP disarms; don't actually restart networking
    await clickText(page, 'Static');
    await sleep(500);
    if (await findText(page, 'Restart network?')) throw new Error('mode toggle did not disarm');
    if (!(await findText(page, 'DHCP'))) throw new Error('mode did not return to DHCP');
    console.log('apply arming + disarm: OK');

    // Timezone: set America/New_York, verify via API, restore UTC
    await focusInput(page, 'UTC');
    for (let i = 0; i < 3; i++) await page.keyboard.press('Backspace');
    await page.keyboard.type('America/New_York');
    await clickText(page, 'Save timezone');
    await sleep(2000);
    if (!(await findText(page, 'Timezone saved'))) throw new Error('timezone save message missing');
    const tz = await page.evaluate(async () => {
        const r = await fetch('/api/network/timezone', { credentials: 'same-origin' });
        return (await r.json()).timezone;
    });
    if (tz !== 'America/New_York') throw new Error(`timezone not applied: ${tz}`);
    await focusInput(page, 'America/New_York');
    for (let i = 0; i < 20; i++) await page.keyboard.press('Backspace');
    await page.keyboard.type('UTC');
    await clickText(page, 'Save timezone');
    await sleep(2000);
    console.log('timezone save via UI: OK');

    // NTP: enable, verify state text flips, then disable. The input
    // may be pre-filled — dntpd_flags persists in rc.conf even when
    // disabled — so clear it based on actual API state.
    const ntpField = async () => {
        const cur = await page.evaluate(async () => {
            const r = await fetch('/api/network/ntp', { credentials: 'same-origin' });
            return (await r.json()).server || '';
        });
        await focusInput(page, cur || 'pool.ntp.org'); // placeholder when unset
        for (let i = 0; i < cur.length; i++) await page.keyboard.press('Backspace');
    };
    await ntpField();
    await page.keyboard.type('pool.ntp.org');
    await clickText(page, 'Save NTP');
    await sleep(2500);
    if (!(await findText(page, 'NTP enabled'))) throw new Error('NTP did not enable');
    if (!(await findText(page, 'NTP server saved'))) throw new Error('NTP save message missing');
    await ntpField();
    await clickText(page, 'Save NTP');
    await sleep(2500);
    if (!(await findText(page, 'NTP disabled'))) throw new Error('NTP did not disable');
    console.log('NTP enable/disable via UI: OK');

    await page.screenshot({ path: '/tmp/flynas-uitest/network-2.png' });
    console.log('ALL OK');
} catch (err) {
    failed = true;
    console.error('FAIL:', err.message);
    await page.screenshot({ path: '/tmp/flynas-uitest/fail.png' });
}
await browser.close();
process.exit(failed ? 1 : 0);
