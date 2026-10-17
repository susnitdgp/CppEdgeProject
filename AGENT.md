Role & Context: You are an expert in developing an institutional-grade C++ XTS API trading bot.
Goal: High-frequency/algorithmic trading bot executing automated quantitative strategies.
Libraries: C++20 / IXWebSocket for WebSocket (µWebSockets has no working client) / simdjson for JSON parsing / redis++ / libcurl for the web client / quill for logging / moodycamel concurrentqueue for inter-thread queues
Package manager: CMake (FetchContent for IXWebSocket, quill, concurrentqueue, redis++; system packages for the rest)
Start with the CRUDEOILM Nov 26 contract (CRUDEOILM26NOVFUT, ExchangeInstrumentID 573423) with 1 lot.

# Project requirements
- C++ code should be optimal: no duplicate, redundant or bloated code. Always check that.
- For any API reference use https://symphonyfintech.com/xts-trading-front-end-api-v2/ and https://symphonyfintech.com/xts-market-data-front-end-api-v2/
- Build a multithreaded trading bot:
  - First thread listens to the WebSocket for one instrument and passes the tick data to another thread using a moodycamel queue.
  - Second thread aggregates the ticks into 3/5-minute candles.
  - Third thread runs the strategy on candles and generates trade signals.
  - Fourth thread executes the order engine and keeps a record of order state.
  - Fifth thread listens to order updates and updates the Redis state accordingly.
- Keep every thread in a separate .hpp file.
- Use JSON for configuration.
- Use Redis for all state management.

# Redis
Redis has a password. It lives only in `~/.config/xts-nautilas/redis.env` (mode 600) and is never committed.
Load it in a shell with
`set -a; . ~/.config/xts-nautilas/redis.env; set +a` before running the bot binaries or redis-cli.

# Layout (`include/edge/`)
- `core/` — shared basics: `types.hpp` (Tick, Candle, Signal, OrderUpdate, queues), `config.hpp` (config.json), `json.hpp` (simdjson helpers), `log.hpp` (quill)
- `net/` — transports: `rest_client.hpp` (libcurl), `socketio_client.hpp` (Socket.IO over IXWebSocket)
- `xts/` — XTS specifics: `session.hpp` (logins, socket URLs), `history.hpp` (OHLC history for warm-up and backtest)
- `store/` — `redis_store.hpp` (credentials in, bot state out)
- `strategy/` — framework: `strategy.hpp` (interface), `pine.hpp` (Pine building blocks), `session.hpp` (calendar), `trade_rules.hpp` (cross + session -> target, shared with the backtest), `factory.hpp` (name -> strategy); each strategy in its own folder `strategy/<name>/<name>.hpp` (`alpha_trend/`, `obv_macd/`) with settings in `config/strategies/<name>.json`
- `threads/` — one header per thread, numbered as above: `t1_market_data`, `t2_candle_builder`, `t3_strategy_runner`, `t4_order_engine`, `t5_order_updates`
- `src/main.cpp` — wires the queues and starts the five threads
- `tools/` — `backtest.cpp` (same strategy + trade rules as the bot), `rest_probe.cpp`
- `config/` — `config.json`, `engine-config.json`, `strategies/<name>.json`, `session-mcx.json`, `backtest.json`
