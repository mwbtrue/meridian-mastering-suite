# Rules

Hard rules promoted from patterns and failures. A lesson becomes a rule when it has shown up twice, or once with a severe cost. Agents must follow these and state which ones apply at session start.

| ID | Rule | Promoted from | Date |
|---|---|---|---|
| R-001 | A performance target is met only when measured on the target platform and compiler (Windows/MSVC Release for FL Studio). Linux results alone are `PARTIAL`. | F-001 (seen twice: after the first fix and again after the second) | 2026-11-21 |
| R-002 | Every optimization ships with a correctness test for the code it changed (null, bit-identical, or tolerance test). | P-008 (`600a68f`, `a50a065`) | 2026-11-21 |
| R-003 | Platform-specific warnings and costs must be caught on every platform: build with `/W4` or `-Wshadow`, and profile per module on Windows. | F-003, F-005 | 2026-11-21 |
| R-004 | Never trust "it ran." Verify with numbers: LUFS, true peak, null depth, latency in samples, CPU percentiles. | P-003, P-004 | 2026-11-21 |
| R-005 | Any change to oversampling, phase mode, or lookahead must update `computeChainLatency()` and pass `chain_null_test`. | P-002, P-005 | 2026-11-21 |

## Promotion template

```markdown
| R-XXX | <rule in one sentence, imperative> | <F-xxx / P-xxx with evidence> | <YYYY-MM-DD> |
```

Retire a rule by moving it to a "Retired" section with the reason and date. Don't delete it.
