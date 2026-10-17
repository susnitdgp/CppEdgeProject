# CppEdgeProject

A multithreaded C++20 algorithmic trading bot for the Symphony **XTS** APIs (CompositeEdge), starting with
**CRUDEOILM26NOVFUT** (MCX, ExchangeInstrumentID 573423), 1 lot, MIS.

Current version: **0.6.0** (see `CMakeLists.txt`).

## How it works

Five threads, one job each, connected by lock-free moodycamel queues:

```
 XTS Market Data socket                                           XTS Interactive socket
          │                                                                 │
          ▼                                                                 ▼
 T1 market data ──Tick──▶ T2 candle builder ──Candle──▶ T3 strategy    T5 order updates
                                                            │                │
                                                         Signal         OrderUpdate
                                                            ▼                ▼
                                                      T4 order engine ◀─────┘
                                                            │
                                         REST: place orders, poll broker positions
```

| Thread | Header | What it does |
|---|---|---|
| T1 | `threads/t1_market_data.hpp` | Logs in to Market Data, subscribes to 1501 Touchline, emits a Tick on every trade (TotalTradedQuantity moved) |
| T2 | `threads/t2_candle_builder.hpp` | Builds 3/5-minute bars on exchange time; closes quiet bars by clock; skips the partial first bar |
| T3 | `threads/t3_strategy_runner.hpp` | Runs the configured strategy on each bar; emits a Signal when the target position changes |
| T4 | `threads/t4_order_engine.hpp` | Target-position loop: one marketable limit order at a time for the difference between target and position |
| T5 | `threads/t5_order_updates.hpp` | Interactive socket: writes order state to Redis and passes updates to T4 |

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
├── xts/       session.hpp (logins, socket URLs)
├── store/     redis_store.hpp (credentials in, bot state out)
├── strategy/  strategy.hpp (interface) · ema_cross.hpp · factory.hpp
└── threads/   t1_market_data · t2_candle_builder · t3_strategy_runner · t4_order_engine · t5_order_updates
src/main.cpp            wires the queues and starts the five threads
config/config.json      what to trade and where
config/engine-config.json  how the engine runs (intervals, timeouts, backoff)
utility/xts_ia_login.sh daily Interactive login -> Redis
tools/rest_probe.cpp    REST client test against XTS (read-only)
run.sh                  starts the bot with pre-flight checks
AGENT.md                project brief and coding rules
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

This produces `build/cpp_edge` (the bot) and `build/rest_probe` (the REST test).

## Daily routine

```bash
cd ~/CppEdgeProject
./utility/xts_ia_login.sh   # once a day: browser login, paste the response; stores the session in Redis
./run.sh                    # starts the bot (Ctrl-C stops all threads cleanly)
```

Run inside tmux so the bot survives an SSH disconnect: `tmux new -s edge './run.sh'`.

`run.sh` loads the Redis password, checks Redis and today's Interactive session, builds the binary if missing,
then starts the bot. The NIFTY key set: `XTS_KEYS=nifty ./utility/xts_ia_login.sh`.

**Market Data sessions:** XTS allows one session per key. The bot logs in with the same Market Data keys as the
Rust XTS bot, so do not run both at once.

## Configuration

### `config/config.json` — what to trade

| Section | Fields |
|---|---|
| `xts` | `host`, `client_id`, `live_orders_enabled` |
| `instrument` | `symbol`, `exchange_instrument_id`, `segment_code`, `segment`, `product`, `lots`, `qty_per_lot` (MCX 1, NIFTY 65), `tick_size`, `protect_points` |
| `candle` | `timeframe_minutes` |
| `strategy` | `name` (`ema_cross`), `fast`, `slow` |
| `redis` | `host`, `port`, `db`, `password_env`, the credential key names, `state_prefix` |
| `logging` | `dir`, `level` |

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

Both files are read at start-up; restart the bot after a change (no rebuild needed).

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

`build/rest_probe [config/config.json] [idle_seconds]` — read-only test of the libcurl client against XTS
Interactive (profile and balance, 1.1 s apart to respect the 1 call/s per-endpoint limit). With `idle_seconds`,
it also times a call after an idle gap to show whether the connection stayed warm.

## XTS notes

- Market Data and Interactive sockets are Socket.IO v2 (Engine.IO v3). The client pings every `pingInterval`;
  the server also pings and drops the socket after ~10 s unless each ping is answered.
- Interactive REST allows 1 call per second per endpoint (HTTP 429 `e-apirl-0004` otherwise).
- XTS speaks HTTP/1.1 only.
- An expired Interactive token shows as `e-session-0005`; the Interactive socket then connects but never sends
  `joined` and is dropped every 10 s. Fix: run `utility/xts_ia_login.sh`.

## Status

Working: both logins, both sockets, 1501 subscription, order-update socket, REST client, position
reconciliation. Not yet tested with live MCX ticks (built on a weekend) or live orders.

Next:
- strategy history warm-up (the EMA cross starts cold and is only a placeholder)
- limit price from the live quote instead of the bar close
- order-book reconciliation after an Interactive socket reconnect
