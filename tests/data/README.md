Test fixtures, synthesized for this repo (no third-party content):

- `tone_44k_stereo.ogg`: 0.5 s, 44.1 kHz, Ogg Vorbis. Left 440 Hz, right 660 Hz.
- `tone_22k_mono.ogg`: 0.25 s, 22.05 kHz, mono, 440 Hz.

Encoded once with libvorbis 1.3.7 from generated sine waves.

`fuzz/`: inputs the fuzzers (tests/fuzz) found that crashed or misbehaved, kept so the tests in `test_*.cpp` that read
them stay fixed without a fuzzer:

- `bmp_zero_height.bmp`: a BMP 0 pixels high, which decoded "successfully" into an empty picture.
- `tga_huge_header.tga`: 25 bytes whose header claims a picture of about 24000 x 23000 pixels.
- `jpeg_idct_overflow.jpg`: a JPEG whose coefficients overflow int in stb_image's IDCT.
- `font_upside_down_box.ttf`: a font with a glyph whose box has its corners swapped, which made stb allocate a
  negative size for its distance field.
