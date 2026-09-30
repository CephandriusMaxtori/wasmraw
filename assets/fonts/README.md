# Bundled fonts

## SpaceGrotesk-Subset.ttf

Latin subset of **Space Grotesk**, static Regular (weight 400), used for the menu
bar. Licensed under the SIL Open Font License 1.1 - see `OFL.txt`.

Upstream: <https://github.com/google/fonts/tree/main/ofl/spacegrotesk>

`SpaceGrotesk-Subset.ttf` is generated, not hand-edited. To rebuild it from the
upstream variable font and regenerate the embedded C array:

```powershell
Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/google/fonts/main/ofl/spacegrotesk/SpaceGrotesk%5Bwght%5D.ttf' -OutFile sg-var.ttf
python -m pip install fonttools

# The upstream axis defaults to Light (300), so pin it to Regular explicitly.
python -m fontTools.varLib.instancer sg-var.ttf wght=400 -o sg-400.ttf

python -m fontTools.subset sg-400.ttf --output-file=SpaceGrotesk-Subset.ttf `
  --unicodes="U+0020-007E,U+00A0-00FF,U+2010-2015,U+2018-201A,U+201C-201E,U+2020-2022,U+2026,U+2030,U+2039,U+203A,U+2044,U+20AC,U+2122,U+2190-2193,U+2212" `
  --layout-features="kern,liga,calt,ccmp,locl" --no-hinting --desubroutinize

python tools/ttf_to_header.py assets/fonts/SpaceGrotesk-Subset.ttf app/third_party/space_grotesk_data.h
```

The instancer does not rewrite the name table, so verify the result reports
`Space Grotesk Regular` and not `Light` before committing it.
