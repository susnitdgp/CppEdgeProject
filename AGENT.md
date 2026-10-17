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
