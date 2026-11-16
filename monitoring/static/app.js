"use strict";
const $ = id => document.getElementById(id);
const el = (tag, text, cls) => {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (cls) node.className = cls;
  return node;
};
let socket, retryTimer, watchdog, configured = false, connected = false;
let watched = [], selected = "", available = [], quotes = new Map(), depth = null;
let received = 0, lastMessage = 0, retryDelay = 1000;
const price = value => {
  if (value === undefined || value === null) return "—";
  const n = BigInt(value), negative = n < 0n, magnitude = negative ? -n : n;
  return (negative ? "−" : "") + (magnitude / 10000n).toLocaleString() + "." + (magnitude % 10000n).toString().padStart(4, "0");
};
const size = value => value === undefined ? "—" : BigInt(value).toLocaleString();
function status(event) {
  if (!connected) return ["Disconnected", "offline"];
  if (!event || !event.available) return ["Awaiting data", "offline"];
  if (event.source_stale) return ["Feed stale", "warn"];
  if (!event.state.book_current) return ["Partial book", "warn"];
  if (!event.state.trade_history_current) return ["Trade history gap", "warn"];
  return ["Current", ""];
}
function setConnection(text, good = false) {
  connected = good;
  $("connection").textContent = text;
  $("connection-dot").className = "dot" + (good ? " green" : "");
  render();
}
function showBanner(message) {
  $("banner").hidden = !message;
  $("banner").textContent = message || "";
}
function render() {
  $("watched-count").textContent = watched.length;
  $("event-count").textContent = received.toLocaleString();
  const body = $("quotes");
  body.replaceChildren();
  for (const symbol of watched) {
    const event = quotes.get(symbol), data = event?.state?.data;
    const row = el("tr");
    row.dataset.symbol = symbol;
    row.tabIndex = 0;
    row.setAttribute("aria-label", "Show depth for " + symbol);
    const choose = () => { selected = symbol; refreshOptions(); connect(); };
    row.addEventListener("click", choose);
    row.addEventListener("keydown", e => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); choose(); } });
    row.append(el("td", symbol));
    for (const side of ["bid", "ask"]) {
      const cell = el("td", price(data?.[side]?.price_1e4), side === "bid" ? "bid-text" : "ask-text");
      cell.append(el("small", data?.[side] ? size(data[side].quantity) + " shares" : "No quote"));
      row.append(cell);
    }
    row.append(el("td", data?.bid && data?.ask ? price(BigInt(data.ask.price_1e4) - BigInt(data.bid.price_1e4)) : "—"));
    const last = el("td", price(data?.last?.price_1e4));
    last.append(el("small", data?.last ? size(data.last.quantity) + " shares" : "No retained trade"));
    row.append(last);
    const cell = el("td"), [label, cls] = status(event);
    cell.append(el("span", label, "status " + cls));
    if (event?.state?.loss_seen) cell.append(el("small", "Loss recorded"));
    if (data && data.trading_status !== "T") cell.append(el("small", "Trading status: " + data.trading_status));
    row.append(cell);
    if (event && Date.now() - event.arrived < 700) row.classList.add("flash");
    body.append(row);
  }
  if (!watched.length) {
    const row = el("tr"), cell = el("td", "Subscribe to a security to see its live quotes.", "empty");
    cell.colSpan = 6; row.append(cell); body.append(row);
  }
  const data = depth?.state?.data, bids = data?.bids || [], asks = data?.asks || [];
  $("depth-name").textContent = selected ? "/ " + selected : "/ select a security";
  $("depth-state").textContent = selected ? status(depth)[0] : "L2";
  $("depth-state").className = "status " + status(depth)[1];
  $("best-bid").textContent = price(bids[0]?.price_1e4);
  $("best-ask").textContent = price(asks[0]?.price_1e4);
  $("spread").textContent = bids.length && asks.length ? price(BigInt(asks[0].price_1e4) - BigInt(bids[0].price_1e4)) : "—";
  $("bid-levels").textContent = bids.length + " levels";
  $("ask-levels").textContent = asks.length + " levels";
  const shown = [...bids.slice(0, 20), ...asks.slice(0, 20)];
  const largest = shown.reduce((m, level) => BigInt(level.quantity) > m ? BigInt(level.quantity) : m, 1n);
  for (const [side, levels] of [["bids", bids], ["asks", asks]]) {
    const table = $(side); table.replaceChildren();
    for (const level of levels.slice(0, 20)) {
      const row = el("tr");
      row.style.setProperty("--fill", Number(BigInt(level.quantity) * 10000n / largest) / 100 + "%");
      const fields = side === "bids" ? [size(level.orders), size(level.quantity), price(level.price_1e4)] : [price(level.price_1e4), size(level.quantity), size(level.orders)];
      fields.forEach(value => row.append(el("td", value)));
      table.append(row);
    }
  }
  $("depth-empty").hidden = !!shown.length;
  $("depth-empty").textContent = !selected ? "Choose an L2 security to inspect the book." : !depth?.available ? "Waiting for " + selected + "…" : "The book has no live price levels.";
}
function refreshOptions(preserveDraft = false) {
  const symbols = [...new Set([...available.map(item => item.symbol), ...watched, ...(selected ? [selected] : [])])].sort();
  const select = $("depth-symbol"), draft = select.value; select.replaceChildren();
  const empty = el("option", "No L2 subscription"); empty.value = ""; select.append(empty);
  symbols.forEach(symbol => { const option = el("option", symbol); option.value = symbol; select.append(option); });
  select.value = preserveDraft && (draft === "" || symbols.includes(draft)) ? draft : selected;
  $("symbol-count").textContent = available.length;
  const list = $("available"); list.replaceChildren();
  available.forEach(item => {
    const button = el("button", undefined, "symbol-button" + (watched.includes(item.symbol) ? " selected" : ""));
    button.type = "button"; button.append(el("span", item.symbol));
    button.append(el("small", item.source_stale ? "STALE" : watched.includes(item.symbol) ? "WATCHING" : "+ ADD"));
    button.onclick = () => {
      if (!watched.includes(item.symbol) && watched.length >= 255) {
        $("form-error").textContent = "Watch at most 255 L1 securities."; return;
      }
      if (!watched.includes(item.symbol)) watched.push(item.symbol);
      selected = item.symbol; configured = true; $("symbols").value = watched.join(", ");
      refreshOptions(); connect();
    };
    list.append(button);
  });
  if (!available.length) list.append(el("p", "Waiting for processor data…", "muted"));
}
function connect() {
  clearTimeout(retryTimer); clearInterval(watchdog);
  const old = socket; socket = null; if (old) old.close();
  quotes.clear(); depth = null;
  setConnection("Connecting");
  const interval = Number($("interval").value);
  $("interval-badge").textContent = interval >= 1000 ? interval / 1000 + " s / security" : interval + " ms / security";
  const ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");
  socket = ws; lastMessage = Date.now();
  ws.onopen = () => { if (socket === ws) ws.send(JSON.stringify({l1: watched, l2: selected ? [selected] : [], interval_ms: interval})); };
  ws.onmessage = event => {
    if (socket !== ws) return;
    lastMessage = Date.now();
    const data = JSON.parse(event.data);
    if (data.type === "subscribed") { retryDelay = 1000; setConnection("Streaming", true); return; }
    if (data.type === "heartbeat") return;
    if (data.type === "error") { showBanner(data.message); return; }
    if (!["snapshot", "update"].includes(data.type)) return;
    data.arrived = Date.now(); received++;
    if (data.stream === "l1") quotes.set(data.symbol, data);
    if (data.stream === "l2" && data.symbol === selected) depth = data;
    const current = [...quotes.values(), ...(depth ? [depth] : [])];
    if (current.some(event => event.available && event.source_stale)) showBanner("Processor updates have stopped. Displayed values are stale.");
    else if (current.some(event => event.state && !event.state.book_current)) showBanner("A subscribed book is incomplete. Check the state column before using its quotes.");
    else if (current.some(event => event.available)) showBanner("");
    $("last-delivery").textContent = "RECEIVED " + new Date().toLocaleTimeString();
    render();
  };
  ws.onclose = () => {
    if (socket !== ws) return;
    clearInterval(watchdog); setConnection("Reconnecting");
    showBanner("Stream disconnected. Reconnecting for a fresh snapshot…");
    retryTimer = setTimeout(connect, retryDelay); retryDelay = Math.min(retryDelay * 2, 5000);
  };
  ws.onerror = () => ws.close();
  watchdog = setInterval(() => { if (Date.now() - lastMessage > 12000) ws.close(); }, 1000);
}
$("watch-form").onsubmit = event => {
  event.preventDefault();
  const symbols = [...new Set($("symbols").value.split(",").map(value => value.trim()).filter(Boolean))];
  if (symbols.length > 255 || symbols.some(symbol => !/^[\x21-\x7e]{1,8}$/.test(symbol))) {
    $("form-error").textContent = "Use up to 255 symbols of 1–8 characters without spaces."; return;
  }
  $("form-error").textContent = "";
  watched = symbols; selected = $("depth-symbol").value; configured = true;
  refreshOptions(); connect();
};
async function poll() {
  try {
    const responses = await Promise.all([fetch("/api/symbols"), fetch("/api/health")]);
    if (responses.some(response => !response.ok)) throw new Error("Service unavailable");
    const [symbols, health] = await Promise.all(responses.map(response => response.json()));
    available = symbols; refreshOptions(true);
    $("fresh-count").textContent = health.fresh_symbols + " / " + health.symbols;
    $("udp-count").textContent = (health.udp_invalid + health.udp_incomplete).toLocaleString();
    if (!configured && symbols.length) {
      watched = symbols.slice(0, 6).map(item => item.symbol); selected = watched[0];
      $("symbols").value = watched.join(", "); configured = true; refreshOptions(); connect();
    }
    if (!health.symbols) showBanner("Waiting for a live processor. Start one with monitoring enabled.");
    else if (!health.fresh_symbols) showBanner("Processor updates have stopped. Displayed values are stale.");
    else if (connected) showBanner(available.some(item => watched.includes(item.symbol) && !item.book_current) ? "A subscribed book is incomplete. Check the state column before using its quotes." : "");
  } catch {
    $("fresh-count").textContent = "—";
    showBanner("Monitoring service unavailable. Waiting to reconnect…");
  } finally { setTimeout(poll, 2000); }
}
setInterval(() => { $("clock").textContent = new Date().toLocaleTimeString(); }, 1000);
$("clock").textContent = new Date().toLocaleTimeString();
connect(); poll();
