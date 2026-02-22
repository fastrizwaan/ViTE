# Encoding Samples

This folder contains sample text files encoded with every encoding currently exposed by ViTE.

## Contents

- `generate_samples.sh`: Regenerates all sample files.
- `files/sample.<encoding-id>.txt`: One file per encoding ID.
- `files/MANIFEST.tsv`: Generation summary (`encoding_id`, iconv target, bytes, mode, decode check).
- `files/VALIDATION.txt`: Validation notes.

## Regenerate

```sh
./samples/encodings/generate_samples.sh
```

## Quick verification in ViTE

1. Open a sample file, e.g. `samples/encodings/files/sample.windows-1251.txt`.
2. Use the status bar Encoding selector (searchable list) to set the matching encoding.
3. Confirm text renders correctly.
4. Save and reopen to verify roundtrip.

Notes:
- UTF-16/UTF-32 samples are generated with BOM to help automatic detection.
- Some legacy encodings use transliteration fallback during sample generation when strict conversion is not possible for all characters (see `MANIFEST.tsv` `convert_mode` column).
