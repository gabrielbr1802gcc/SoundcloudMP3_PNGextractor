# SoundcloudMP3_PNGextractor

This is a small, dependency-free C++ tool that extracts the embedded cover art (thumbnail) from an MP3 file using ID3v2 tags. It supports ID3v2.2 (`PIC`) and ID3v2.3/2.4 (`APIC`).

Some MP3 downloads (for example, from SoundCloud/downloaders) store the cover inside the tag in a way that Windows/Android doesn’t always show clearly. This program pulls that image out and writes it as a normal image file.

## Usage

### Interactive mode (nice inside Code::Blocks)

Run the program, paste the MP3 path, and press Enter. Then either type an output path or just press Enter to use the default.

### Command line

Extract cover next to the MP3:

```bash
thumb_extrac "C:\\Musics\\teste.mp3"
```

Extract cover into a folder:

```bash
thumb_extrac "C:\\Musics\\teste.mp3" "D:\\Capas\\"
```

Debug mode (prints tag/frame details):

```bash
thumb_extrac --debug "C:\\Musics\\teste.mp3"
```

Notes: paths with spaces are fine (use quotes). If you paste a path that already includes quotes, the program will handle it.

## Default output

If you don’t provide an output path, the image is written next to the MP3 using the MP3 name plus a `_cover` suffix, with the image extension based on the embedded data (jpg/png/etc.).
