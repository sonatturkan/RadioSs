# mAirList 3.1 CSV adapter

This adapter implements the **historical mAirList 3.1** library CSV layout in
`mAirList-3.1-User-Manual.pdf`, PDF pages 127–128, figure 11.1 (revision 19,
6 July 2011). It does not claim support for current mAirList versions or native
database files.

The file has no header and exactly 20 columns:

| No. | Field | No. | Field |
| --- | --- | --- | --- |
| 1 | Internal mAirList ID | 11 | Ramp 2 |
| 2 | External ID | 12 | Ramp 3 |
| 3 | Full path/file name | 13 | Hook In |
| 4 | Title | 14 | Hook Fade |
| 5 | Artist(s) | 15 | Hook Out |
| 6 | Ending | 16 | Outro |
| 7 | Duration | 17 | Start Next |
| 8 | Comment | 18 | Fade Out |
| 9 | Cue In | 19 | Cue Out |
| 10 | Ramp 1 | 20 | Anchor |

For import, column 1 must be empty and column 3 must contain a path. Duration
and all cue positions are decimal **seconds from the start of the file**.
A single hyphen means “keep the existing value” in the manual's update format.
External IDs are allowed to repeat in mAirList; repeated values are reported
as identity ambiguities for SDBASE and are never silently collapsed into one
record.

## Preview

```python
from sdbase.mairlist31_csv import preview_mairlist31_csv

report = preview_mairlist31_csv(
    "library.csv", source_namespace="mairlist31:studio-a")
for entry in report["entries"]:
    print(entry["row"], entry["status"], entry["proposals"])
print(report["ambiguities"], report["losses"])
```

The preview preserves all 20 raw fields, reports malformed rows, converts
decimal seconds to proposed integer milliseconds, and marks every cue for
human review. Ramp 1–3, Hook Fade and Anchor have no automatic SDBASE cue
meaning. Ending and Comment have no canonical SDBASE destination in this
adapter. Submillisecond rounding appears in the loss report. The preview
never reads audio, writes `.sdb`, or approves media/cues; `can_apply` is always
`false`.

The default text encoding is UTF-8 with optional BOM. For an older Windows
export, specify the known encoding explicitly, such as `encoding="cp1252"`.
The reader does not guess an encoding. Input is bounded to 16 MiB, 50,000 rows,
8,192 characters per field and seven days for numeric time values.

## Apply reviewed rows as drafts

```python
from sdbase.mairlist31_import import apply_mairlist31_csv

preview = preview_mairlist31_csv(
    "library.csv", source_namespace="studio-a")
result = apply_mairlist31_csv(
    store, principal, station_id, "library.csv",
    source_namespace="studio-a",
    expected_source_sha256=preview["source_sha256"],
    idempotency_key="migration-batch-001")
```

The apply service requires the station's `import` permission and binds the
reviewed SHA-256 to the exact CSV bytes. One transaction creates draft items,
draft versions, ineligible station links, external identity mappings and an
import job. Each mapping retains all 20 raw fields, field states, proposed
millisecond values, cue review status, row number, source file hash and loss
report. No media file or approved radio cue is created. A repeated apply with
the same key and source hash returns the prior result without writes. A later
row with changed data for the same source External ID conflicts.

Apply is deliberately narrower than the historical CSV format: every row
must have a nonempty title and External ID; External IDs and file paths must
be unique within the file and cannot conflict with prior imports in the same
station namespace. mAirList permits duplicate External IDs, so those files
remain **preview only** until an operator supplies an explicit identity
mapping in a future workflow. Apply is bounded to 1 MiB and 1,000 rows per
atomic batch. A rejected batch writes nothing.

The CLI exposes `mairlist31-preview` and `mairlist31-apply`; run
`sdbase <command> --help` for arguments. The Python API above is suitable for
applications that already hold an authenticated `Principal`.

## Selected-view export

```python
from sdbase.mairlist31_export import export_mairlist31_csv
from sdbase.search import search_items

view = search_items(store, principal, station_id, title="Example")
report = export_mairlist31_csv(
    store, principal, station_id, view["items"],
    {media_file_id: "/absolute/path/to/audio.wav"})
if report["skipped"] or report["losses"]:
    print(report["skipped"], report["losses"])
csv_text = report["csv_text"]
```

Export rechecks each selected item, approved version and verified media file;
it requires an explicit absolute path for that media. It writes column 2 as
the SDBASE item UUID and leaves the mAirList internal ID blank. It exports
only approved cues bound to the selected media hash. Any missing or ambiguous
field is reported. The CSV cannot express SDBASE rights, approval evidence,
station associations, media hashes or audit history, and the loss report says
so. Audio bytes are not copied and supplied paths are not checked on the
destination machine. Export is bounded to 50,000 selected rows and 16 MiB of
UTF-8 CSV text.
