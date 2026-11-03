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

## Documentation & Knowledge Management

Documentation is a mandatory part of the development workflow. The project must maintain three complementary documents:

### 1. `README.md` — Project Documentation

**Purpose:** Explain what the project does, how it works, and how to use it.

Keep the README updated with:

- Project overview and current development status
- Completed, ongoing, and planned milestones
- Technology stack and dependencies
- Project directory structure
- Architecture overview and component responsibilities
- Data flow between implemented components
- Build, test, and execution instructions
- Current features, limitations, and known issues
- Performance measurements, when available and verified

Explain how components interact using concise technical descriptions and diagrams where useful.

The README must always represent the **actual implemented state** of the project, not planned functionality presented as completed.

### 2. `docs/DECISIONS.md` — Engineering Decision Log

**Purpose:** Record why important technical decisions were made.

For each significant architectural or implementation decision, document:

- The problem or requirement
- Options considered
- Selected approach
- Reasoning and tradeoffs
- Alternatives rejected and why
- Performance, correctness, or reliability implications
- Conditions that would justify revisiting the decision

Do not document trivial implementation details or create unnecessary entries.

Preserve historical decisions. If a decision changes, mark it as superseded and explain why rather than silently rewriting history.

### 3. `docs/LEARNING.md` — C++ & Systems Engineering Learning Journal

**Purpose:** Help me understand the codebase and develop the technical knowledge needed to build and maintain it independently.

For every milestone, document:

- Important C++ language features introduced
- Relevant systems programming concepts
- Networking, memory management, concurrency, and performance concepts
- How these concepts are used in the actual implementation
- Important bugs encountered and their root causes
- Lessons learned during code reviews
- Common pitfalls and how to avoid them

Use small, relevant code snippets from the project to illustrate concepts when useful.

Prioritize understanding **why the code works**, not merely describing what each line does.

Do not turn this into a generic C++ textbook. Focus on concepts directly encountered in the project.

### 4. Mandatory Documentation Workflow

Before every meaningful code commit:

1. Review the code changes.
2. Update `README.md` if implementation, behavior, architecture, dependencies, or usage changed.
3. Update `docs/DECISIONS.md` if important engineering decisions were introduced or revised.
4. Update `docs/LEARNING.md` with meaningful concepts, debugging insights, or lessons encountered.
5. Ensure all documentation accurately reflects the resulting code.
6. Include relevant documentation updates in the same commit as the code changes.

Do not create artificial documentation changes for trivial modifications.

### 5. Pull Request Requirements

Every milestone PR must contain a **Documentation Summary** explaining:

- Which documentation files were updated
- What technical decisions were recorded
- What learning concepts were added
- Whether any documentation remains incomplete

Documentation correctness is part of the milestone acceptance criteria.

### 6. Documentation Quality Standards

- Prefer clear, concise, technically accurate explanations.
- Reference actual project classes, functions, and files.
- Never claim unimplemented functionality is complete.
- Never invent benchmark results or test outcomes.
- Avoid duplicating content across documentation files.
- Keep documentation synchronized with the latest implementation.
- Treat documentation as part of production-quality engineering, not an afterthought.

### 7. Code-Verified Learning Documentation

`docs/LEARNING.md` must be treated as a technically accurate, code-verified learning resource, not an AI-generated summary.

#### Requirements

1. **Verify explanations against the actual codebase.**
   - Inspect the relevant source files before documenting a concept.
   - Ensure explanations accurately describe how the concept is implemented.
   - Never assume a language feature, design pattern, or optimization is used without verifying it.

2. **Use real code examples whenever possible.**
   - Prefer short snippets extracted from the actual implementation.
   - Reference the corresponding file, function, or class.
   - Do not invent code examples and present them as existing project code.
   - Clearly label hypothetical examples as illustrative.

3. **Verify technical correctness.**
   - Cross-check explanations against C++ language rules and relevant library documentation.
   - Distinguish guaranteed behavior from implementation-specific behavior and assumptions.
   - Avoid oversimplifications that introduce incorrect technical claims.
   - Clearly state limitations, undefined behavior risks, and important edge cases.

4. **Explain concepts in context.**
   - Explain what the concept is.
   - Identify where it appears in our project.
   - Explain why we use it and what problem it solves.
   - Discuss important tradeoffs, alternatives, and pitfalls.
   - Include relevant code-review findings and lessons learned.

5. **Prioritize understanding over documentation volume.**
   - Focus on concepts genuinely encountered during implementation.
   - Avoid generic textbook explanations unrelated to the codebase.
   - Avoid documenting speculative optimizations as completed work.

#### Mandatory Verification Before Every Commit

Whenever `docs/LEARNING.md` is modified:

1. Reinspect the referenced source files.
2. Verify that code snippets match the implementation.
3. Verify that technical claims are accurate.
4. Check for contradictions with `README.md` and `docs/DECISIONS.md`.
5. Correct outdated explanations when the implementation changes.
6. Include a **Learning Documentation Verification** summary in the PR description.

### 8. Source-Code Traceability

Every technical concept documented in `docs/LEARNING.md` must be traceable to the actual source code.

#### Requirements

1. **Reference the implementation**
   - Include the relevant source file and function, class, or method.
   - Prefer clickable relative Markdown links to source files.
   - Use line numbers only when useful, since they may become outdated after refactoring.
   - Verify that referenced files, symbols, and links actually exist.

2. **Use verified code snippets**
   - Prefer code excerpts copied from the current implementation.
   - Ensure snippets match the codebase at the time of writing.
   - Clearly label simplified or hypothetical examples.
   - Never present pseudocode as actual implementation.

3. **Explain each concept in context**
   - **What:** What is the C++ or systems concept?
   - **Where:** Where is it implemented in the project?
   - **Why:** Why was this approach chosen?
   - **How:** How does the implementation work?
   - **Pitfalls:** What could go wrong, and how does our code address it?

4. **Maintain references during refactoring**
   - Update source links and explanations when implementations change.
   - Remove or revise examples that no longer match the code.
   - Ensure learning documentation reflects the current codebase, while preserving significant historical lessons.

5. **Verify before committing**
   - Check that every referenced source file and symbol exists.
   - Confirm that code snippets accurately represent the implementation.
   - Identify hypothetical examples explicitly.
   - Report any documentation that could not be verified.
