# SoundcloudMP3_PNGextractor

This is a small, dependency-free C++ tool that extracts the embedded cover art (thumbnail) from an MP3 file using ID3v2 tags. It supports ID3v2.2 (`PIC`) and ID3v2.3/2.4 (`APIC`).

Some MP3 downloads (for example, from SoundCloud/downloaders) store the cover inside the tag in a way that Windows/Android doesn’t always show clearly. This program pulls that image out and writes it as a normal image file.

## Usage

### Interactive mode (menu)

Run the program without arguments and choose:

- `1) Single`: extract from one MP3
- `2) Batch`: extract from all `.mp3` files inside a folder

In both modes you can:

- choose the output folder / output file
- choose whether overwriting existing files is allowed

### Command line

Old-style (still supported):

```bash
thumb_extrac "C:\\Music\\test.mp3"
```

Extract cover into a folder:

```bash
thumb_extrac "C:\\Music\\test.mp3" "D:\\Covers\\"
```

New-style commands:

Single (one MP3), choose name and allow overwrite:

```bash
thumb_extrac single "C:\\Music\\test.mp3" --name "{stem} - cover" --overwrite
```

Batch (all MP3 files inside a folder), output to a folder:

```bash
thumb_extrac batch "C:\\Music\\" --out-dir "D:\\Covers\\" --pattern "{stem}"
```

Debug mode (prints tag/frame details):

```bash
thumb_extrac --debug "C:\\Music\\test.mp3"
```

Help:

```bash
thumb_extrac --help
```

Notes: paths with spaces are fine (use quotes). If you paste a path that already includes quotes, the program will handle it.

## Default output

If you don’t provide an output path, the image is written next to the MP3 using the MP3 filename (`{stem}`), with the image extension based on the embedded data (jpg/png/etc.).

## Overwrite protection

By default the program will NOT overwrite existing output files.

- interactive mode: it asks if you want to overwrite
- command line: use `--overwrite` to allow overwriting
