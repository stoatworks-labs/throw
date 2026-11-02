// The page's half of the crosscheck: fetch the cases and the harness's card
// from crosscheck.py's server, render each case through
// window.throwDemo.renderCase, and post the picture back, with the page's
// table of controls and any uniform the page set that a program lacks.
// crosscheck.py compares; nothing is judged here.
const post = (path, body) => fetch(path, { method: 'POST', body });

async function main() {
  for (let i = 0; i < 200 && !window.throwDemo; i += 1) await new Promise((r) => setTimeout(r, 50));
  if (!window.throwDemo) throw new Error('window.throwDemo never appeared: the page did not start');
  const job = await (await fetch('/xc/job.json')).json();
  const frame = new Uint8Array(await (await fetch('/xc/card.rgba')).arrayBuffer());
  await post('/xc/table', JSON.stringify(window.throwDemo.table));
  const missing = new Set();
  for (const c of job.cases) {
    const out = await window.throwDemo.renderCase({
      settings: c.settings, frame, width: job.width, height: job.height,
    });
    out.missing.forEach((name) => missing.add(name));
    await post(`/xc/out/${c.name}`, out.pixels);
  }
  await post('/xc/missing', JSON.stringify([...missing]));
  await post('/xc/format', window.throwDemo.formats.surface);
  await post('/xc/done', 'ok');
}

main().catch((error) => post('/xc/error', String(error?.stack ?? error)));
