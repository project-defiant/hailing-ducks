# Examples

Runnable `.sql` walkthroughs, using only the fixtures already committed under `test/` — no external
data, credentials, or network access needed. Run from the repo root after `make release`:

```sh
./build/release/duckdb < examples/raw_scanners.sql
./build/release/duckdb < examples/ld_query_walkthrough.sql
```

| File | Demonstrates |
|------|--------------|
| `raw_scanners.sql` | `hail_scan_blockmatrix` / `hail_scan_table` — full sequential scans over a `.bm`/`.ht` directory. |
| `ld_query_walkthrough.sql` | The batch-optimized LD query pipeline end to end: preflight → HT resolution → `hail_ld_materialize`, including a not-found variant and a flipped-allele match. |

For real PanUKBB S3 data instead of the synthetic fixtures, see the root [README.md](../README.md#real-s3-data) and `make test_s3_smoke`. For the full LD query schema/status-code/policy reference, see [docs/LD-QUERY.md](../docs/LD-QUERY.md).
