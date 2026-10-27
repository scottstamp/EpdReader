# Lists web editor (Web Bluetooth demo)

Edits the reader's to-do / grocery lists from a desktop browser over Bluetooth LE.
A stand-in for the future mobile app; it speaks the same protocol.

## Run

1. On the reader, open **Lists** from the home carousel. Bluetooth is on only while a
   Lists screen is open.
2. Serve this folder over `localhost` (Web Bluetooth only works on secure origins, and
   `localhost` counts as one):

   ```
   python -m http.server 8000 --directory tools/lists_web
   ```

3. Open <http://localhost:8000> in **Chrome or Edge** (desktop), press **Connect** and
   pick "nRF EPD Reader".

Edit lists and items, then **Save to device**. Checking items on the reader updates the
page live; if you have unsaved edits at that moment the page offers to reload instead.
**Demo** opens the editor with sample data and no device.

## How it works

- `lists.js` holds the document format and the Bluetooth protocol (no browser APIs
  outside `ListsClient`), mirroring `src/lists_doc.h` and `src/lists_ble.h`.
- The page downloads the whole lists document, edits it, and writes it back with
  confirmed writes. Each save carries the revision it was based on; if the reader's
  lists changed in between, the save is refused and the page asks you to reload.
- Limits (enforced on both sides): 16 lists, 128 items in total, 96 bytes per name or
  item, ~3 KB of text.

## Tests

```
node tools/lists_web/test/test_web.mjs
```

Needs `gcc` on `PATH`. Runs the C unit tests for `src/lists_doc.c`, checks that the
JavaScript and C parsers agree on hand-written and 200 random documents, and runs
`ListsClient` against a fake device that follows the firmware's rules (chunking,
conflicts, invalid documents, timeouts).
