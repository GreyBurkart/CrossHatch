# TheList: one-way Apple Notes transfer

Keep the Apple Note titled **TheList** as the source of truth. CrossHatch stores
one offline copy at **`/CrossHatch/Notes/TheList.md`**. The title identifies the
note in the sender; firmware identifies it by this fixed SD path, not an Apple
account or private Notes ID.

The implementation reuses `CrossPointWebServer` and `CrossPointWebServerActivity`
for foreground transfer, `HalStorage` for SD I/O, and `ReaderActivity` dispatch
to `ChecklistActivity` or `TxtReaderActivity`/`MarkdownReaderLayout`.
`src/util/SyncedNote.*` owns only the fixed path and replacement/recovery steps;
the existing File Transfer and Pinned doc Quick Actions need no new settings.

## On the X4 Pro

1. Choose **File Transfer** from Home or an existing Quick Action, then **Join
   Network** or **Create Hotspot**. Keep that screen open during transfer.
   Home → **TheList** also opens File Transfer if no copy has arrived yet.
2. Send the file using the HTTP request below. The directory is created for you.
3. Exit File Transfer and choose Home → **TheList**. No connection is needed now.

Notes with tasks open in the existing interactive checklist: touch a row or
select it with buttons and Confirm. Completed rows have a filled checkbox and
**Done** label. **View Markdown text** opens the full formatted note, including
prose between tasks. Notes without tasks open directly in the Markdown reader.
The existing **Pinned doc** Quick Action can also target this file.

## Upload contract

Use the address displayed by File Transfer, including its port if shown:

```sh
curl --fail-with-body -F 'file=@TheList.md;filename=TheList.md;type=text/markdown' \
  'http://crosspoint.local/upload?path=/CrossHatch/Notes'
```

Send one UTF-8 file as **multipart/form-data**, field `file`, filename
`TheList.md`. Put the directory in the URL query, not a form field. A raw text
POST, JSON body, or Apple Notes collaboration link is not a file upload.
Success is HTTP 200 with `File uploaded successfully: TheList.md`.

Only this exact HTTP destination gains replacement semantics. Generic HTTP
uploads and WebSocket uploads retain their existing collision behavior; the
browser's WebSocket uploader is not the repeat-send path for this MVP.
The maximum incoming note is 64 KiB. An empty note intentionally clears the
copy. Unsupported Markdown is accepted as text.

An upload streams through the existing 4 KiB buffer into `.upload.tmp`; only
after writing, syncing, and closing does it replace the previous file. Failed
or interrupted transfers retain the previous copy. A `.upload.bak` supports
recovery if interrupted between renames; the next Home open or HTTP upload
attempts recovery. Physical SD/power-loss behavior still requires device testing.
Re-sending replaces **all** local changes, including checkbox completion.

## Expected Apple Shortcut

This is a recipe, not an installed or device-verified Shortcut:

1. Receive an exported Markdown file through the Share Sheet, or select the
   Apple Note whose title is exactly **TheList**. If multiple notes match,
   select the intended account/folder instead of silently taking the first.
2. Export/convert its contents to UTF-8 Markdown. Preserve checked versus
   unchecked tasks as `- [x]` and `- [ ]`. Check a sample containing both states;
   a plain-text conversion that loses state cannot reconstruct it afterward.
3. Set the output file name to `TheList.md`.
4. Use **Get Contents of URL**, method **POST**, body **Form**, with a `file`
   field of type **File** holding that renamed file, and the URL above.
   Let Shortcuts supply the multipart boundary.
5. Display the response; report success only after a successful upload.

Apple documents Markdown export on
[Mac](https://support.apple.com/guide/notes/not201900c07/mac) and
[iPhone](https://support.apple.com/guide/iphone/iphdf551cfa2/ios), plus
[POST form/file bodies in Shortcuts](https://support.apple.com/guide/shortcuts-mac/apd58d46713f/mac).
On supported systems, exporting Markdown first is the concrete fallback if
direct Note-to-Markdown conversion is unavailable. The exact Share Sheet input,
native checklist serialization, and iPad workflow remain to be verified on the
user's Apple devices. No private Apple APIs are needed by the firmware.

## Format and boundaries

- Checklist: ATX headings (`#`–`######`), unordered or ordered task rows,
  `[ ]`, `[x]`, and `[X]`; tasks inside code fences/front matter/comments are ignored.
- Full note view: headings, paragraphs, lists, emphasis, quotes, code, link
  labels, and rules through the existing restrained Markdown parser. Tables,
  attachments, images, and Apple-specific formatting are not faithfully rendered.
- Interactive limits: 128 headings/tasks, 8 KiB of labels, 255-byte heading/task
  lines. Exceeding these offers the full Markdown text view without toggles.
- Single file, manual foreground transfer, no background service, accounts,
  two-way merge, or new reading statistics. Local checks never update Apple Notes.
- No EPUB/cache format change or manual cache reset is needed.

Before expanding, decide whether multiple notes need separate Home entries,
whether mixed prose and checkboxes need one combined view, and which verified
Apple export/Shortcut path to support. Local checks remain disposable by design.

## Hardware check

On X4 Pro, upload a note with headings, prose, one checked and one unchecked
task. Exit transfer, open **TheList**, toggle a row, view the full text, and reopen
offline. Re-send changed content and verify it replaces the local check. Confirm
an interrupted or over-limit upload leaves the old copy readable. Exercise both
Join Network and Hotspot; verify ordinary file upload and reading still work.
