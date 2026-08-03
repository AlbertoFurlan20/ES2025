# Report fragments

Design-rationale fragments written as they are decided, to be assembled into
`report.tex` later. Writing them at decision time keeps the reasoning (and the
numbers that motivated it) accurate, rather than reconstructing it at submission.

Each fragment is self-contained Markdown, states which `report.tex` section it
feeds, and cites its sources inline so the citation survives the LaTeX
conversion.

## Index

| Fragment | Feeds report section |
|----------|----------------------|
| [01-temperature-decoupling-rationale.md](01-temperature-decoupling-rationale.md) | Design and implementation |
| [02-conversion-timing-defect-evidence.md](02-conversion-timing-defect-evidence.md) | Design and implementation; Problems encountered |

## Conventions

- **Cite precisely.** Datasheet references give document revision and section
  (`BST-BMP180-DS000-09 Rev 2.5, §3.3`). Code references give `file:line`.
- **Quote verbatim.** Where a design choice rests on a vendor statement, quote it
  rather than paraphrasing — the paraphrase is what gets challenged in a review.
- **Keep the numbers.** Measured and derived figures go in the fragment with the
  conditions under which they hold. A number without its conditions is not
  evidence.
- **Separate decision from justification.** State what was chosen, then why. A
  reader should be able to disagree with the reasoning while still understanding
  the choice.
