import createOrderBook from './orderbook.mjs';

const $ = (id) => document.getElementById(id);
const fmt = new Intl.NumberFormat('en-US');
const TAPE_LIMIT = 60;

const Module = await createOrderBook();
const session = new Module.Session();

function render(result) {
  const { book } = result;
  drawSide($('asks'), [...book.asks].reverse(), 'ask', book);
  drawSide($('bids'), book.bids, 'bid', book);

  const bestBid = book.bids[0]?.[0];
  const bestAsk = book.asks[0]?.[0];
  if (bestBid !== undefined && bestAsk !== undefined) {
    $('spread').textContent = `spread ${bestAsk - bestBid} tick${bestAsk - bestBid === 1 ? '' : 's'}`;
    $('mid').textContent = `bid ${fmt.format(bestBid)}  ·  ask ${fmt.format(bestAsk)}`;
  } else {
    $('spread').textContent = bestBid !== undefined || bestAsk !== undefined ? 'one-sided book' : 'no quotes';
    $('mid').textContent = bestBid === undefined && bestAsk === undefined
      ? 'Place an order or load the example book'
      : bestBid !== undefined ? `bid ${fmt.format(bestBid)} · no asks` : `no bids · ask ${fmt.format(bestAsk)}`;
  }

  $('st-orders').textContent = fmt.format(book.orders);
  $('st-trades').textContent = fmt.format(book.totalTrades);
  $('st-pool').textContent = `${fmt.format(book.poolInUse)} / ${fmt.format(book.poolCapacity)} slots`;
  for (const e of result.events ?? []) addTape(e);
}

function drawSide(el, levels, kind, book) {
  const max = Math.max(1, ...book.asks.map((l) => l[1]), ...book.bids.map((l) => l[1]));
  el.replaceChildren(...levels.map(([price, qty, orders]) => {
    const row = document.createElement('button');
    row.type = 'button';
    row.className = `level ${kind}`;
    row.style.setProperty('--fill', `${(qty / max) * 100}%`);
    row.title = `Use price ${price}`;
    row.innerHTML = `<span>${fmt.format(price)}</span><span>${fmt.format(qty)}</span><span>${orders}</span>`;
    row.addEventListener('click', () => {
      document.querySelector('#order-form [name=price]').value = price;
      document.querySelector(`#order-form [name=side][value=${kind === 'ask' ? 'BUY' : 'SELL'}]`).checked = true;
    });
    return row;
  }));
}

function addTape(e) {
  const li = document.createElement('li');
  li.className = e.type;
  const text = {
    trade: () => `<b>TRADE</b> ${fmt.format(e.qty)} @ ${fmt.format(e.price)} <small>#${e.aggressor} ${e.side.toLowerCase()} hit #${e.resting}</small>`,
    rested: () => `<b>RESTED</b> #${e.id} ${e.side.toLowerCase()} ${fmt.format(e.qty)} @ ${fmt.format(e.price)}`,
    cancelled: () => `<b>CANCELLED</b> #${e.id} <small>${fmt.format(e.qty)} unfilled</small>`,
    rejected: () => `<b>REJECTED</b> #${e.id} <small>${e.reason}</small>`,
    note: () => e.text,
  }[e.type]();
  li.innerHTML = text;
  const tape = $('tape');
  tape.prepend(li);
  while (tape.children.length > TAPE_LIMIT) tape.lastChild.remove();
}

function call(fn) {
  try {
    const result = JSON.parse(fn());
    render(result);
    return result;
  } catch (err) {
    addTape({ type: 'note', text: `Error: ${err.message}` });
    return null;
  }
}

$('order-form').addEventListener('submit', (ev) => {
  ev.preventDefault();
  const f = new FormData(ev.target);
  const type = f.get('type');
  const price = type === 'MARKET' ? 0 : Number(f.get('price'));
  call(() => session.submit(f.get('side'), type, price, Number(f.get('qty'))));
});

document.querySelector('#order-form [name=type]').addEventListener('change', (ev) => {
  document.querySelector('#order-form [name=price]').disabled = ev.target.value === 'MARKET';
});

function manageValues() {
  const f = new FormData($('manage-form'));
  return { id: Number(f.get('id')), price: f.get('price'), qty: f.get('qty') };
}

$('cancel-btn').addEventListener('click', () => {
  const { id } = manageValues();
  if (id > 0) call(() => session.cancel(id));
});

$('modify-btn').addEventListener('click', () => {
  const { id, price, qty } = manageValues();
  if (!(id > 0) || price === '' || qty === '') {
    addTape({ type: 'note', text: 'Modify needs an order id, a new price and a new quantity.' });
    return;
  }
  call(() => session.modify(id, Number(price), Number(qty)));
});

// A small two-sided book to play with.
$('example-btn').addEventListener('click', () => {
  session.reset();
  $('tape').replaceChildren();
  const orders = [
    ['SELL', 10105, 40], ['SELL', 10104, 25], ['SELL', 10103, 60], ['SELL', 10102, 15], ['SELL', 10101, 30],
    ['SELL', 10101, 20], ['BUY', 10099, 35], ['BUY', 10099, 10], ['BUY', 10098, 50], ['BUY', 10097, 20],
    ['BUY', 10096, 45], ['BUY', 10095, 30],
  ];
  let last;
  for (const [side, price, qty] of orders) last = JSON.parse(session.submit(side, 'LIMIT', price, qty));
  render({ ...last, events: [] });
  addTape({ type: 'note', text: 'Loaded 12 resting orders (#1–#12). Try a buy at 10102 for 70 to sweep two levels.' });
  document.querySelector('#order-form [name=price]').value = 10102;
  document.querySelector('#order-form [name=qty]').value = 70;
});

for (const btn of document.querySelectorAll('[data-sim]')) {
  btn.addEventListener('click', async () => {
    const n = Number(btn.dataset.sim);
    btn.disabled = true;
    btn.textContent = 'Running…';
    await new Promise((r) => setTimeout(r, 20)); // let the label repaint
    const r = call(() => session.simulate(n));
    btn.disabled = false;
    btn.textContent = `Simulate ${fmt.format(n)} orders`;
    if (!r) return;
    const perSec = r.micros > 0 ? Math.round(r.simulated / (r.micros / 1e6)) : 0;
    $('st-sim').textContent = `${fmt.format(r.simulated)} orders in ${(r.micros / 1000).toFixed(1)} ms (${fmt.format(perSec)}/s)`;
    addTape({
      type: 'note',
      text: `Simulated ${fmt.format(r.simulated)} orders: ${fmt.format(r.trades)} trades, ${fmt.format(r.tradedQty)} shares, ${(r.micros / 1000).toFixed(1)} ms of matching.`,
    });
  });
}

$('reset-btn').addEventListener('click', () => {
  session.reset();
  $('tape').replaceChildren();
  $('st-sim').textContent = 'not run yet';
  call(() => session.snapshot());
});

call(() => session.snapshot());
$('app').setAttribute('aria-busy', 'false');
