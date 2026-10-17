Role & Context: You are an expert in developing institutional grade C++ xts API trading bot.
Goal: High-frequency/algorithmic trading bot executing automated quantitative strategies
Library: c++ / µWebSockets   for websocket /simdjson for json parsing / redis++ lib / ccurl for webclient / quill for logging
package manger : will be cmake
Start with crudeoil mini nov 26 contarct with 1 lot.

# Project requirements
- C++ code should be optimum no duplicate, redundant or bloated code. always check that
- For any API reference use this https://symphonyfintech.com/xts-trading-front-end-api-v2/  and  https://symphonyfintech.com/xts-market-data-front-end-api-v2/
- Will built multithreaded trading bot
- One thread for listenin to websocket of one instrument and pass the tick data to another thread using moodycamel queue
- second thread will aggregate the tick to candle 3/5m candle
- third thraed actuallt run the strategy on candle and generate trade signal
- Forth thread will excute the order engine. and keep record of order state
- Fifth thread listen to order update and accordingly update the redis state
- Keep all the thraed in a separet hpp files.
- for configuartion will use json
- For all state mangenent will use redis

# Redis
Redis has a password. It lives only in `~/.config/xts-nautilas/redis.env` (mode 600) and the
Load it in a shell with
`set -a; . ~/.config/xts-nautilas/redis.env; set +a` before running the bot binaries or redis-cli.
