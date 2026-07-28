-- Raw HailTable/BlockMatrix scanner walkthrough.
--
-- Run from the repo root, after `make release`:
--   ./build/release/duckdb < examples/raw_scanners.sql
--
-- These two functions are full sequential scanners: they read every row/element and are the right
-- tool for inspection, conversion, or joining Hail data with other tables in SQL. For resolving a
-- fine-mapping locus's variants against a real HailTable/BlockMatrix without a full scan, see
-- examples/ld_query_walkthrough.sql instead.

.echo on

-- hail_scan_blockmatrix(path): one row per matrix element, (row_idx, col_idx, value).
SELECT COUNT(*) AS n_elements FROM hail_scan_blockmatrix('test/matrix.bm');

SELECT MIN(row_idx), MAX(row_idx), MIN(col_idx), MAX(col_idx)
FROM hail_scan_blockmatrix('test/matrix.bm');

-- Diagonal elements only.
SELECT row_idx, value
FROM hail_scan_blockmatrix('test/matrix.bm')
WHERE row_idx = col_idx
ORDER BY row_idx
LIMIT 5;

-- hail_scan_table(path): one row per HailTable row, schema derived from the table's own Hail type
-- metadata. Nested Array/Struct fields become DuckDB LIST/STRUCT; Locus(GENOME) becomes
-- STRUCT(contig VARCHAR, position INTEGER).
SELECT COUNT(*) AS n_rows FROM hail_scan_table('test/hailtable_fixture.ht');

SELECT locus.contig, locus.position, alleles
FROM hail_scan_table('test/hailtable_fixture.ht')
LIMIT 5;
