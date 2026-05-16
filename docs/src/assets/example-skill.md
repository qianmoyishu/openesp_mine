# Weather

Get current weather and forecasts through `cap_cli`. Support weather, forecast, temperature queries.

## When to use
When the user asks about weather, temperature, forecasts.

## How to use
1. Use `cap_cli` to run `time --now` and learn the current date
2. Use `cap_cli` to run `web_search --query "weather in [city] today"`
3. Extract temperature, conditions, and forecast from results
4. Present in a concise, friendly format

## Example
User: "What's the weather in Tokyo?"
-> cap_cli: `time --now`
-> cap_cli: `web_search --query "weather Tokyo today February 2026"`
-> "Tokyo: 8C, partly cloudy. High 12C, low 4C. Light wind from the north."
