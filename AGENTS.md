# Market Data Engine — Development Instructions

## Project Objective

Build a low-latency C++20 market-data engine that ingests live exchange data, reconstructs a local order book, and measures processing performance.

The primary objective is to develop deep understanding of C++, networking, memory management, concurrency, and profiling.

## Engineering Principles

1. Prefer simple, correct implementations before optimization.
2. Follow YAGNI and avoid unnecessary abstractions.
3. Do not introduce additional frameworks or dependencies without justification.
4. Preserve market-data ordering, correctness, and synchronization guarantees.
5. Use reproducible benchmarks to justify performance changes.
6. Keep the performance-critical path observable and testable.

## Learning-First Workflow

Before implementing a significant component:

1. Explain the engineering problem.
2. Present relevant design alternatives and tradeoffs.
3. Identify the underlying C++ and systems concepts.
4. Ask me to make or approve the important decisions.
5. Create a small implementation plan with acceptance criteria.

During implementation:

- Make small, reviewable changes.
- Use tests to verify correctness.
- Explain nontrivial code and memory ownership.
- Do not implement additional features outside the agreed scope.
- Flag architectural changes before proceeding.

After implementation:

- Summarize the changes.
- Report test results and what was not tested.
- Identify possible performance bottlenecks without assuming them.
- Ask me technical questions to verify my understanding.
- Recommend the next smallest useful task.

## AI Assistance Boundaries

Codex may implement boilerplate, build configuration, tests, and routine infrastructure.

For performance-critical algorithms, concurrency primitives, custom memory management, and significant architectural decisions, prioritize explanation and guided implementation rather than immediately generating complete solutions.

Use Ponytail to challenge unnecessary complexity and Matt Pocock's Skills for structured engineering workflows.

Project-specific requirements and correctness take precedence over generic simplification advice.
