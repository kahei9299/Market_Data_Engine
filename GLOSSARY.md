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

## Order Book

**Best ask** — The lowest price on the ask side of the order book. Undefined when the ask side is empty.

**Best bid** — The highest price on the bid side of the order book. Undefined when the bid side is empty.

**Depth snapshot** — A REST API response (`GET /api/v3/depth`) containing the complete order book state at a point in time. Contains `lastUpdateId`, bids, and asks. Limited to 5,000 price levels per side. Not a differential update — it replaces the entire book.

**Local order book** — An in-memory representation of the exchange's order book for a single symbol, maintained by applying a depth snapshot followed by differential depth updates. Only correct while synchronized with the exchange's update stream.

**Resynchronization** — The process of discarding the local order book and restarting from a fresh snapshot, triggered by a sequence gap, snapshot failure, or other unrecoverable synchronization error.

**Sequence continuity** — The invariant that each depth update's first update ID (`U`) equals the previous update's last update ID (`u`) plus one. A violation means updates were lost and the local order book is no longer correct.

**Update ID** — A monotonically increasing integer identifying a position in the Binance update stream. Each depth update covers a contiguous range `[U, u]`. Each depth snapshot has a `lastUpdateId`. The local order book tracks the last applied update ID to detect stale or missing events.
