# Glossary

Domain terms for the market-data-engine project. Use these terms consistently in code, documentation, and discussion.

## Market Data

**Absolute quantity** — The quantity provided by Binance for a price level is the new total quantity at that price, not a delta. Receiving `["78450.06", "0.5"]` means "the quantity at 78450.06 is now 0.5 BTC," not "add 0.5 BTC."

**Deletion** — A price level with zero quantity (`"0.00000000"`). Instructs the consumer to remove that price level from the book if it exists. If the level doesn't exist, the operation is a no-op — not an error.

**Depth update** — A Binance WebSocket event containing incremental changes to bid and ask price levels, with sequence IDs (`U`, `u`) identifying the update range. It is not a complete order-book snapshot.

**Message** — One complete WebSocket text frame containing a JSON object from Binance.

**Price level** — A `[price, quantity]` pair representing the total aggregated quantity at a given price on one side of the order book (bid or ask). A depth update contains the new absolute quantity, not a quantity to add or subtract.

**Stream** — A named Binance channel (e.g., `btcusdt@depth`). One WebSocket connection carries one or more streams.

## Numeric Representation

**Scaled integer** — An integer representing a decimal value multiplied by a fixed power of 10. `"78450.06"` at scale 10^8 becomes `7845006000000`. The scale must be explicitly defined, and conversion must reject unsupported precision and overflow.
