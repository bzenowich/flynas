// FlyNAS VMs page UI test: create a VM, start it (QEMU/NVMM), watch
// it report running, suspend/resume, stop, then delete.
//
// Preconditions: none (creates + deletes its own VM "uitestvm" with a
// 1 GB disk on the system dir). Cleanest with an empty vms table.
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

    await clickText(page, 'VMs');
    if (!(await waitFor(page, 'Virtual machines')))
        throw new Error('VMs page did not load');
    await page.screenshot({ path: '/tmp/flynas-uitest/vms-1.png' });
    console.log('VMs page renders: OK');
    await sleep(1500);   // let the initial load settle before interacting

    // Enable the NAT network (flynas0 + dnsmasq + pf) and leave it on
    // so the port-forward test below has a loaded pf anchor.
    if (await findText(page, 'Off')) {
        await clickText(page, 'Off');
        await sleep(2500);
    }
    if (!(await findText(page, 'On'))) throw new Error('NAT network did not enable');
    console.log('NAT network enable: OK');

    // Create a small VM. Fill the form, then click Create; the Start
    // button only appears in a real VM row (the name also echoes in
    // the input, so don't assert on it). Retry — the page's 5s refresh
    // can eat a click.
    await clickText(page, 'name');
    await page.keyboard.type('uitestvm');
    await sleep(300);
    await clickText(page, 'vCPU');
    await page.keyboard.type('1');
    await sleep(300);
    await clickText(page, 'RAM MB');
    await page.keyboard.type('256');
    await sleep(300);
    await clickText(page, 'disk GB');
    await page.keyboard.type('1');
    await sleep(300);
    let created = false;
    for (let attempt = 0; attempt < 3 && !created; attempt++) {
        await clickText(page, 'Create');
        await sleep(1500);
        created = await waitFor(page, 'Start', {}, 6);
    }
    if (!created) throw new Error('VM not created (no Start button)');
    console.log('create VM: OK');

    // Port-forward: expand the VM, add 8080 -> 80, then remove it.
    await sleep(800);
    await clickText(page, 'uitestvm');   // name click expands the row
    if (!(await waitFor(page, 'Port forwards', { prefix: true }, 6)))
        throw new Error('VM row did not expand');
    await clickText(page, 'host port');
    await page.keyboard.type('8080');
    await sleep(300);
    await clickText(page, 'guest port');
    await page.keyboard.type('80');
    await sleep(300);
    await clickText(page, 'Forward');
    if (!(await waitFor(page, 'tcp 8080 -> 80', {}, 8)))
        throw new Error('port-forward not added');
    console.log('add port-forward: OK');
    await sleep(800);
    await clickText(page, 'Remove');
    let fgone = false;
    for (let i = 0; i < 8 && !fgone; i++) {
        await sleep(1000);
        fgone = !(await findText(page, 'tcp 8080 -> 80'));
    }
    if (!fgone) throw new Error('port-forward not removed');
    console.log('remove port-forward: OK');
    await clickText(page, 'uitestvm');   // collapse
    await sleep(800);

    // Start -> running (Suspend button appears once running)
    await sleep(500);
    await clickText(page, 'Start');
    if (!(await waitFor(page, 'Suspend', {}, 12)))
        throw new Error('VM did not reach running (no Suspend button)');
    await page.screenshot({ path: '/tmp/flynas-uitest/vms-2.png' });
    console.log('start VM (QEMU/NVMM): OK');

    // Suspend -> Resume
    await sleep(800);
    await clickText(page, 'Suspend');
    if (!(await waitFor(page, 'Resume', {}, 10)))
        throw new Error('suspend did not take');
    console.log('suspend: OK');

    // Resume -> Suspend
    await sleep(800);
    await clickText(page, 'Resume');
    if (!(await waitFor(page, 'Suspend', {}, 10)))
        throw new Error('resume did not take');
    console.log('resume: OK');

    // Stop -> Start (stopped shows Start + Delete)
    await sleep(800);
    await clickText(page, 'Stop');
    if (!(await waitFor(page, 'Start', {}, 12)))
        throw new Error('stop did not take');
    console.log('stop: OK');

    // Delete (two-click)
    await sleep(800);
    await clickText(page, 'Delete');
    await sleep(500);
    if (!(await findText(page, 'Confirm?'))) throw new Error('delete did not arm');
    await clickText(page, 'Confirm?');
    let gone = false;
    for (let i = 0; i < 10 && !gone; i++) {
        await sleep(1000);
        gone = !(await findText(page, 'uitestvm'));
    }
    if (!gone) throw new Error('VM still present after delete');
    console.log('delete VM: OK');

    // Leave the NAT network off (clean state)
    await sleep(800);
    if (await findText(page, 'On')) {
        await clickText(page, 'On');
        await sleep(2500);
        if (!(await findText(page, 'Off'))) throw new Error('NAT network did not disable');
    }
    await page.screenshot({ path: '/tmp/flynas-uitest/vms-3.png' });
    console.log('NAT network disable: OK');
    console.log('ALL OK');
} catch (err) {
    failed = true;
    console.error('FAIL:', err.message);
    await page.screenshot({ path: '/tmp/flynas-uitest/fail.png' });
}
await browser.close();
process.exit(failed ? 1 : 0);
