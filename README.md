# CppEdgeProject

A multithreaded C++20 algorithmic trading bot for the Symphony **XTS** APIs (CompositeEdge), starting with
**CRUDEOILM26NOVFUT** (MCX, ExchangeInstrumentID 573423), 1 lot, MIS.

Current version: **0.9.0** (see `CMakeLists.txt`). Active strategy: **AlphaTrend** (set in `config/config.json`);
**OBV MACD** is also available. Both are ports of TradingView indicators.

## How it works

Six threads, one job each, connected by lock-free moodycamel queues:

```
 XTS Market Data socket                                           XTS Interactive socket
          │                                                                 │
          ▼                                                                 ▼
 T1 market data ──Tick──▶ T2 candle builder ──Candle──▶ T3 strategy    T5 order updates
                                                     (+ XTS history)        │
                                                            │               │
                                                         Signal        OrderUpdate
                                                            ▼               ▼
                                                      T4 order engine ◀─────┘
                                                            │
                                         REST: place orders, poll broker positions

 T1 ──raw copies──▶ T6 recorder ──▶ data/raw/<symbol>/<YYYY-MM-DD>.jsonl.gz   (never blocks trading)
```

| Thread | Header | What it does |
|---|---|---|
| T1 | `threads/t1_market_data.hpp` | Market Data socket: subscribes to 1501 Touchline, emits a Tick on every trade (TotalTradedQuantity moved) |
| T2 | `threads/t2_candle_builder.hpp` | Builds 3/5-minute bars on exchange time; closes quiet bars by clock; skips the partial first bar |
| T3 | `threads/t3_strategy_runner.hpp` | Warms the strategy up on XTS history, fills bar gaps from history, skips off-session bars, turns crosses into target positions via the trade rules |
| T4 | `threads/t4_order_engine.hpp` | Target-position loop: one marketable limit order at a time for the difference between target and position |
| T5 | `threads/t5_order_updates.hpp` | Interactive socket: writes order state to Redis and passes updates to T4 |
| T6 | `threads/t6_recorder.hpp` | Records every subscribed market-data message (1501/1502/1505/1510) and socket status, as received, with our receive time |

`main` logs in to Market Data once; T1's socket and T3's history calls share that session.

### Strategies

Each strategy lives in its own folder, `strategy/<name>/<name>.hpp`, builds itself from the `indicator` section
of `config/strategies/<name>.json` (`from_json`), and reports **Up** / **Down** crosses. Shared Pine building blocks
(EMA/RMA, WMA, stdev, sums with TradingView's na rules) are in `strategy/pine.hpp`. `config.json` picks the strategy:
`"strategy": {"name": "alpha_trend", "params_file": "strategies/alpha_trend.json"}`.

Adding a strategy: create `strategy/<name>/<name>.hpp` (a `Strategy` with a static `from_json`), add
`config/strategies/<name>.json`, and add one line to `kStrategies` in `strategy/factory.hpp`.

#### AlphaTrend (`strategy/alpha_trend/alpha_trend.hpp`)

Port of KivancOzbilgic's "AlphaTrend" (Pine v5):

1. ATR = Hull MA of true range (`period` 14)
2. Momentum: MFI(hlc3, 14) ≥ 50 is bullish (`use_rsi`: RSI(close, 14) instead, for data without volume)
3. The AlphaTrend line ratchets: bullish → `low − ATR × coeff`, only ever up; bearish → `high + ATR × coeff`,
   only ever down
4. **BUY** (Up) when the line crosses above its value 2 bars ago, **SELL** (Down) when it crosses below;
   BUY and SELL alternate, as the chart labels do

It only remembers a short window, so after warm-up its signals should match TradingView on the same contract.

#### OBV MACD (`strategy/obv_macd/obv_macd.hpp`)

Port of the TradingView "OBV MACD Indicator" (Pine v4), MA type DEMA:

1. OBV (`cum(sign(change(close)) × volume)`), its deviation from a 14-bar SMA rescaled into price points
   → a price-like OBV line
2. MACD = DEMA(OBV line, 9) − EMA(close, 26)
3. Linear-regression end value over 2 bars (with length 2 this equals the MACD itself)
4. T-Channel (Alex Grover): a step line that moves only when the MACD leaves it by more than the running
   average distance; its direction is blue (up) or red (down)
5. **Up cross** (turns blue) and **Down cross** (turns red) are the signals

Pine semantics are kept (EMA seeded with an SMA, population stdev, `cum` skips na). The T-Channel threshold
averages over every bar since the first one, so values depend on how much history is loaded: T3 warms up on
`history_days` of XTS bars at start-up.

### Trade rules (`strategy/trade_rules.hpp`, shared by the bot and the backtest)

- **Up cross:** exits a short; enters long if `allow_long` and the session allows entries.
- **Down cross:** exits a long; enters short if `allow_short` and the session allows entries.
- Both sides allowed = the cross reverses; one side = that side's entry, exit on the next cross.
- **Session** (`config/session-mcx.json`): no entries in the last `entry_cutoff_before_close_min` (30) of the
  day, flat at `square_off_before_close_min` (20) before the close — 23:00 / 23:10 now, 23:25 / 23:35 with the
  winter close from 2 Nov. Holidays and special sessions come from the calendar's `overrides`.
- A position still open on a new session day is flattened (MIS does not carry over).
- No stop loss: exits are the opposite cross and the square-off.

### Order engine rules

- **Position of record** is `edge:position:<id>` in Redis. It is read before every order and written on every fill.
- **One working order at a time.** XTS has no reduce-only, so two working orders could both fill.
- **Marketable limit orders:** signal price ± `protect_points`, rounded to `tick_size`.
- **No automatic re-send.** A rejected or cancelled order is not retried; the next signal decides.
- **Reconciliation:** every `reconcile_interval_ms` (30 s) the engine reads the broker's net position for the
  instrument and product. If it differs from Redis while no order is working, **the broker wins** and Redis is
  corrected. Each check is recorded in `edge:recon`. The same call keeps the REST connection warm.
- **Stale order warning** when a working order has no final update after `stale_order_ms` (60 s).
- **Live orders are off** until `xts.live_orders_enabled` is `true` in `config/config.json`.
- Orders are tagged with the `CE` prefix (`orderUniqueIdentifier`), so the bot ignores other orders on the account.

## Layout

```
include/edge/
├── core/      types.hpp (Tick, Candle, Signal, OrderUpdate, queues) · config.hpp · json.hpp · log.hpp
├── net/       rest_client.hpp (libcurl) · socketio_client.hpp (Socket.IO over IXWebSocket)
├── xts/       session.hpp (logins, socket URLs) · history.hpp (OHLC history)
├── store/     redis_store.hpp (credentials in, bot state out)
├── strategy/  strategy.hpp (interface) · pine.hpp · session.hpp (calendar) · trade_rules.hpp · factory.hpp
│   ├── alpha_trend/alpha_trend.hpp
│   └── obv_macd/obv_macd.hpp
└── threads/   t1_market_data · t2_candle_builder · t3_strategy_runner · t4_order_engine · t5_order_updates · t6_recorder
src/main.cpp                    wires the queues and starts the six threads
config/config.json              what to trade and where; names the strategy and session files
config/engine-config.json       how the engine runs (intervals, timeouts, backoff)
config/strategies/<name>.json   per-strategy settings, long/short switches, warm-up days
config/session-mcx.json         MCX calendar + entry / square-off rules
config/backtest.json            backtest data range, costs, outputs
utility/xts_ia_login.sh         daily Interactive login -> Redis
tools/backtest.cpp              backtest with the bot's own strategy and trade rules
tools/rest_probe.cpp            REST client test against XTS (read-only)
run.sh                          starts the bot with pre-flight checks
AGENT.md                        project brief and coding rules
```

## Libraries

| Purpose | Library | Source |
|---|---|---|
| WebSocket (WSS) | IXWebSocket 11.4.6 (µWebSockets has no working client) | fetched by CMake |
| JSON | simdjson | apt `libsimdjson-dev` |
| HTTP REST | libcurl | apt `libcurl4-openssl-dev` |
| Redis | redis++ 1.3.15 + hiredis | fetched + apt `libhiredis-dev` |
| Logging | quill 10.0.1 | fetched by CMake |
| Queues | moodycamel concurrentqueue 1.0.4 | fetched by CMake |
| TLS | OpenSSL | apt `libssl-dev` |

## Build

One-time system packages:

```bash
sudo apt update && sudo apt install -y libcurl4-openssl-dev libhiredis-dev libsimdjson-dev libssl-dev ninja-build
```

Build (Release, C++20):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

This produces `build/cpp_edge` (the bot), `build/backtest` and `build/rest_probe`.

## Daily routine

```bash
cd ~/CppEdgeProject
./utility/xts_ia_login.sh   # once a day: browser login, paste the response; stores the session in Redis
./run.sh                    # starts the bot (Ctrl-C stops all threads cleanly)
```

Run inside tmux so the bot survives an SSH disconnect: `tmux new -s edge './run.sh'`.

`run.sh` loads the Redis password, checks Redis and today's Interactive session, builds the binary if missing,
then starts the bot. The NIFTY key set: `XTS_KEYS=nifty ./utility/xts_ia_login.sh`.

**Market Data sessions:** XTS allows one session per key. The bot and the backtest (with `"source": "xts"`) log
in with the same Market Data keys as the Rust XTS bot, so do not run them at the same time as the Rust bot.

## Configuration

### `config/config.json` — what to trade

| Section | Fields |
|---|---|
| `xts` | `host`, `client_id`, `live_orders_enabled` |
| `instrument` | `symbol`, `exchange_instrument_id`, `segment_code`, `segment`, `product`, `lots`, `qty_per_lot` (MCX 1, NIFTY 65), `tick_size`, `protect_points` |
| `candle` | `timeframe_minutes` |
| `strategy` | `name` (`alpha_trend` or `obv_macd`), `params_file` (relative to `config/`) |
| `session_file` | the session calendar (relative to `config/`) |
| `redis` | `host`, `port`, `db`, `password_env`, the credential key names, `state_prefix` |
| `logging` | `dir`, `level` |

### `config/strategies/alpha_trend.json` — AlphaTrend

| Field | Default | Meaning |
|---|---|---|
| `indicator.coeff` | 1.0 | ATR multiplier for the line's distance from price |
| `indicator.period` | 14 | ATR (Hull) and MFI/RSI period |
| `indicator.use_rsi` | false | RSI instead of MFI (for data without volume) |
| `trade.allow_long` / `allow_short` | true / false | which sides are traded |
| `history_days` | 30 | XTS history fed at start-up (warm-up) |

### `config/strategies/obv_macd.json` — OBV MACD

| Field | Default | Meaning |
|---|---|---|
| `indicator.window_len` | 28 | stdev window for OBV deviation and bar range |
| `indicator.v_len` | 14 | OBV SMA length |
| `indicator.obv_len` | 1 | EMA of the OBV line (1 = none) |
| `indicator.ma_len` | 9 | DEMA length (MACD fast line) |
| `indicator.slow_len` | 26 | EMA of close (MACD slow line) |
| `indicator.reg_len` | 2 | linear-regression length (≥ 2) |
| `indicator.channel_mult` | 1.0 | T-Channel threshold multiplier (higher = fewer flips) |
| `trade.allow_long` / `allow_short` | true / false | which sides are traded |
| `history_days` | 30 | XTS history fed at start-up (warm-up) |

### `config/session-mcx.json` — session calendar

`regular` open/close, `overrides` by date (`null` = closed), `weekends_closed`, `timezone_offset_minutes` (330),
and `rules`: `entry_start_after_open_min`, `entry_cutoff_before_close_min`, `square_off_before_close_min`
(relative to each day's close). Bars outside the calendar (XTS sends stray ones) never reach the strategy.

### `config/engine-config.json` — how the engine runs

| Field | Default | Meaning |
|---|---|---|
| `queue_poll_ms` | 100 | Consumer wake-up to check for shutdown |
| `candle.close_grace_ms` | 1500 | Close a quiet bar this long after its end |
| `order_engine.reconcile_interval_ms` | 30000 | Broker position check (also keeps REST warm) |
| `order_engine.stale_order_ms` | 60000 | Warn when a working order has no final update |
| `rest.connect_timeout_ms` / `request_timeout_ms` | 3000 / 5000 | libcurl timeouts |
| `rest.tcp_keepidle_s` / `tcp_keepintvl_s` | 30 / 10 | TCP keepalive probes |
| `rest.max_conn_age_s` | 3600 | Max idle age before libcurl stops reusing a connection |
| `rest.dns_cache_s` | 3600 | DNS cache lifetime |
| `socket.reconnect_min_ms` / `reconnect_max_ms` | 1000 / 30000 | WebSocket reconnect backoff |
| `socket.watchdog_tick_ms` | 250 | Ping / dead-link check interval |
| `recorder.enabled` | true | Record raw market data (thread 6) |
| `recorder.dir` | `data/raw` | Output folder (`<dir>/<symbol>/<YYYY-MM-DD>.jsonl.gz`, not committed) |
| `recorder.feeds` | 1501, 1502, 1505, 1510 | XTS feeds subscribed and recorded: touchline, 5-level depth, 1-min candles, open interest |
| `recorder.flush_interval_ms` | 2000 | How often the file is flushed (a crash loses at most this much) |
| `recorder.max_queue` | 200000 | Above this backlog thread 1 drops recorder copies (counted, logged) instead of waiting |

All files are read at start-up; restart the bot after a change (no rebuild needed).

## Backtest

```bash
set -a; . ~/.config/xts-nautilas/redis.env; set +a   # only for "source": "xts"
./build/backtest config/backtest.json
```

It replays bars through the bot's own strategy, session and trade rules, so it trades exactly as the bot would.
Fills are at the deciding bar's close ± slippage (the bot sends a marketable limit at that close).

| Field | Meaning |
|---|---|
| `config` | the bot's `config.json` (instrument, strategy, session) |
| `source` | `xts` = fetch history and save it to `csv_path`; `csv` = re-read `csv_path` (no login) |
| `from`, `to`, `trade_from` | data range (IST dates); bars before `trade_from` only warm the indicator |
| `costs` | `round_trip_points` (taxes/fees per lot), `slippage_points_per_side`, `brokerage_per_order` |
| `point_value` | rupees per point per lot (CRUDEOILM: 10) |
| `trades_csv`, `signals_csv` | outputs; the signals file lists every cross with indicator values, for chart checks |

Results go to `backtest_results/` (not committed).

Results, 15 Sep – 16 Oct 2026, CRUDEOILM Nov 5m, 1 lot, costs included:

| Strategy | Mode | Trades | Win % | Gross pts | Net ₹ | Profit factor | Max DD ₹ |
|---|---|---|---|---|---|---|---|
| AlphaTrend | long | 51 | 39.2 | +788 | **+4,310** | 1.28 | 4,060 |
| AlphaTrend | short | 54 | 25.9 | −433 | −8,110 | 0.61 | 10,130 |
| AlphaTrend | both | 105 | 32.4 | +355 | −3,800 | 0.89 | 8,350 |
| OBV MACD | long | 194 | 39.7 | +639 | −7,190 | 0.81 | 11,530 |
| OBV MACD | short | 192 | 27.1 | −695 | −20,390 | 0.51 | 20,390 |
| OBV MACD | both | 386 | 33.4 | −56 | −27,580 | 0.65 | 30,220 |

One month, one contract, in a strong uptrend (7,800 → 9,400): long-only benefits from that. AlphaTrend trades
~2 times a day against OBV MACD's ~6, which is what lets it beat costs. Test a non-trending period before
going live. Live orders stay off.

## Recorded market data

Thread 6 writes one gzip JSON-lines file per instrument and IST day, one line per message:

```
{"rx":<our receive time, Unix ns>,"ev":"1502-json-full","d":{ ...the XTS payload exactly as sent... }}
{"rx":...,"ev":"socket","d":"connected"}        socket status lines mark gaps in the data
```

Raw messages are kept unchanged so any order-flow rule (trade classification, footprint rows, imbalance
ratios) can be recalculated later. Restarting on the same day appends to the same file. Read with
`zcat data/raw/CRUDEOILM26NOVFUT/2026-10-19.jsonl.gz | head`.

## Redis

Credentials (read by the bot, db 0): `susanta:xts_md_app_key`, `susanta:xts_md_secret_key`, `susanta:xts_ia_token`,
`susanta:xts_ia_user_id` (the last two are written daily by `utility/xts_ia_login.sh`).

Bot state (prefix `edge:`):

| Key | Type | Content |
|---|---|---|
| `edge:position:<id>` | hash | `lots`, `avg_price`, `source` (fill / reconcile), `updated_ms` |
| `edge:order:<uid>` | hash | side, lots, limit, status, cum/leaves qty, avg price, reject reason, AppOrderID |
| `edge:recon` | hash | `broker_lots`, `redis_lots`, `status`, `checked_ms` |
| `edge:trades` | list | raw XTS trade events |

```bash
set -a; . ~/.config/xts-nautilas/redis.env; set +a
redis-cli hgetall edge:position:573423
redis-cli hgetall edge:recon
```

The Redis password lives only in `~/.config/xts-nautilas/redis.env` (mode 600) and is never committed.

## Tools

- `build/backtest [config/backtest.json]` — see Backtest above.
- `build/rest_probe [config/config.json] [idle_seconds]` — read-only test of the libcurl client against XTS
  Interactive (profile and balance, 1.1 s apart to respect the 1 call/s per-endpoint limit). With `idle_seconds`,
  it also times a call after an idle gap to show whether the connection stayed warm.

## XTS notes

- Market Data and Interactive sockets are Socket.IO v2 (Engine.IO v3). The client pings every `pingInterval`;
  the server also pings and drops the socket after ~10 s unless each ping is answered.
- Interactive REST allows 1 call per second per endpoint (HTTP 429 `e-apirl-0004` otherwise).
- XTS speaks HTTP/1.1 only.
- OHLC history: `GET /apimarketdata/instruments/ohlc`, IST time strings (`Oct 13 2026 090000`),
  `compressionValue` in seconds; each bar's time is its last second in IST epoch. One call returns thousands of bars.
- XTS sends occasional off-session data (e.g. a Saturday 15:25 bar); the session calendar filters it out.
- An expired Interactive token shows as `e-session-0005`; the Interactive socket then connects but never sends
  `joined` and is dropped every 10 s. Fix: run `utility/xts_ia_login.sh`.

## Status

Working: both logins, both sockets, 1501 subscription, order-update socket, REST client, position
reconciliation, history warm-up, AlphaTrend and OBV MACD strategies, session rules, backtest. Not yet tested with live MCX ticks
(built on a weekend) or live orders.

Next:
- backtest AlphaTrend on a non-trending period / another contract before live orders
- confirm the ports against TradingView on the same contract (CRUDEOILM Nov, 5m)
- limit price from the live quote instead of the bar close
- order-book reconciliation after an Interactive socket reconnect
