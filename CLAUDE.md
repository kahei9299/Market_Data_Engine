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

Claude may implement boilerplate, build configuration, tests, and routine infrastructure.

For performance-critical algorithms, concurrency primitives, custom memory management, and significant architectural decisions, prioritize explanation and guided implementation rather than immediately generating complete solutions.

Use Ponytail to challenge unnecessary complexity and Matt Pocock's Skills for structured engineering workflows.

Project-specific requirements and correctness take precedence over generic simplification advice.

## Production Engineering Standards

This project should be engineered as a production-oriented, low-latency market-data infrastructure component rather than a demonstration application.

### Core requirements

- Prioritize correctness, reliability, and data integrity before performance optimizations.
- Design explicit lifecycle and failure states for networking and market-data synchronization.
- Use bounded memory and buffering wherever practical.
- Ensure reliable resource cleanup and safe ownership throughout asynchronous operations.
- Validate untrusted exchange data and handle malformed input without undefined behavior.
- Never silently ignore sequence gaps, data loss, or synchronization failures.
- Implement appropriate reconnection, timeout, and recovery mechanisms.
- Include meaningful diagnostics, error reporting, and operational metrics.
- Write automated tests for correctness, error handling, and recovery paths.
- Keep the performance-critical path measurable and avoid unnecessary blocking operations.

### Scalability

Design components with clear responsibilities so that additional market-data feeds, symbols, and processing workers can be supported later.

Do not implement speculative distributed systems, custom allocators, or complex concurrency frameworks before they are required.

### Development process

For every milestone:

1. Define functional and nonfunctional acceptance criteria.
2. Identify important failure scenarios.
3. Implement the smallest production-quality solution satisfying those criteria.
4. Verify behavior using automated tests and appropriate integration tests.
5. Document engineering decisions and tradeoffs.
6. Report remaining limitations honestly.

Use Ponytail to challenge unnecessary complexity and relevant Matt Pocock skills for design, specification, TDD, and code review.

Production-quality does not mean feature-complete. Each milestone must be reliable within its explicitly defined scope.

## Milestone Pull Request Requirements

Every development milestone must be implemented on a dedicated Git branch and submitted as a GitHub pull request.

Do not merge automatically.

### Required PR description

Include:

1. **Objective:** What the milestone accomplishes.
2. **Changes:** Files and components added or modified.
3. **Design decisions:** Important engineering choices and tradeoffs.
4. **Correctness:** Expected behavior, invariants, and edge cases.
5. **Testing:** Tests executed, results, and any untested scenarios.
6. **Performance:** Measurements where relevant; do not invent results.
7. **Known limitations:** Remaining risks and deferred improvements.
8. **Learning review:** Important C++ and systems concepts introduced.

### Review process

- Keep PRs focused on one milestone.
- Do not include unrelated refactoring or future features.
- Ensure the project builds and relevant tests pass before requesting review.
- Run appropriate sanitizers when supported and relevant.
- Wait for external code review before merging.
- Address review findings with focused changes and regression tests where appropriate.
- Never dismiss correctness or reliability findings solely because existing tests pass.
- Do not merge with unresolved P0 or P1 issues.

The objective is production-quality, reviewable, incrementally developed C++ software.
