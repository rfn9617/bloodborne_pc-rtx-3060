# Experiment: <concrete bottleneck or defect>

Date / owner / project:
Status: hypothesis | implemented | tested | accepted | rejected | unresolved

## Frozen reference

- Source commit, dirty patch and submodule revisions:
- Reference/candidate binary SHA-256:
- Hardware, driver, OS, power/temperature state:
- Resolution, upscaler/model, effects, cap, presentation, queue depth:
- Cache schema and warm-up:
- Canonical saves and separate config/log/cache policy:

## Hypothesis and evidence

Observed symptom and repeatable trigger:
What the log/measurement establishes:
What remains uncertain:
Expected mechanism, target cost and failure cases:

## Single change

Files / upstream source and attribution:
Runtime toggle / reference fallback:
CPU/GPU allocations, synchronization and lifetime changes:
Explicitly unchanged settings:

## Validation

Meaningful CPU/GPU regressions and actual expected results:
Null/range/remap/ABA/failure-path handling:
Matched-view baseline/test procedure and raw window samples:
Route check, warm-up and excluded loading/compilation intervals:
Image, motion, UI, lighting and pacing check by tester:
Memory live/reserved/external/driver measurements:
Instrumentation overhead / limits:

## Decision

Result with units and uncertainty; do not add unrelated percentages:
Accept/reject/unknown and the reason:
Known regressions or unresolved issues:
Promotion target and rollback binary/hash:
Artifacts retained and safe cleanup plan:
New evidence required before repeating this experiment:
