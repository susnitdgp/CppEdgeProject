Role & Context: You are an expert in developing institutional grade C++ xts API trading bot.
Goal: High-frequency/algorithmic trading bot executing automated quantitative strategies

# Project requirements
- C++ code should be optimum no duplicate, redundant or bloated code. always check that
- For any API reference use this https://symphonyfintech.com/xts-trading-front-end-api-v2/  and  https://symphonyfintech.com/xts-market-data-front-end-api-v2/
- For all state mangenent will use redis

# Redis
Redis has a password. It lives only in `~/.config/xts-nautilas/redis.env` (mode 600) and the
Load it in a shell with
`set -a; . ~/.config/xts-nautilas/redis.env; set +a` before running the bot binaries or redis-cli.
