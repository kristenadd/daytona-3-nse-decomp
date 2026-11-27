# Logo and icons

The repository ships a generic launcher logo and app icons (a chequered flag,
plain lettering). The game's own logo is Sega's and is not committed. To build
with your own art, keep it outside the repository and point a `branding.ini`
at it. That file is ignored by git, so your pictures stay on your machine.

## What uses which picture

| Key in `branding.ini` | Picture | Used by |
|---|---|---|
| `logo` | the title logo, transparent background, drawn on a white band | the launcher on every platform (built into the binary) |
| `icon` | the app icon, 1024 x 1024, opaque | iPhone / iPad app, the Mac app (`tools/macapp/install_mac_app.sh`) |
| `vision_back` | Vision Pro icon, back layer, 1024 x 1024, opaque | visionOS app |
| `vision_front` | Vision Pro icon, front layer, 1024 x 1024, transparent | visionOS app (floats over the back layer) |

The generic pictures are `src/platform/launcher/logo.png` and the PNGs in
`src/platform/ios/Assets.xcassets` and `Assets.vision.xcassets`.

## 1. Make the pictures

`make_branding.py` turns your art into the whole set. It needs Pillow
(`python3 -m pip install pillow`).

```bash
python3 tools/branding/make_branding.py \
    --logo my-logo.png \
    --icon my-icon.png \
    --icon-back my-icon-background.png \
    --out ../daytona-branding
```

- `--logo` (required): the logo with a transparent background. It is trimmed
  and scaled to fit 980 x 440.
- `--icon`: a square picture for the app icon. Without it, the icon is the
  logo over `--icon-back` (or over the generic background).
- `--icon-back`: the icon's background without the logo. With it, the Vision
  Pro icon gets the logo as its own floating layer; without it, the whole icon
  is the back layer and the front layer is empty.
- `--ico-source`: a picture for the Windows `.ico` (daytona-shim's launcher);
  without it, the icon is used.

It writes `logo.png`, `icon-1024.png`, `icon-vision-back.png`,
`icon-vision-front.png` and `icon.ico` into `--out`. Keep that folder outside
the repository; beside it (`../daytona-branding`) lets daytona-shim use the same set.

`--generic --font third_party/fonts/RussoOne-Regular.ttf` draws the generic set
that is committed.

## 2. Point the build at them

```bash
cp branding.example.ini branding.ini
```

Edit the paths if your folder is elsewhere. Paths are absolute or relative to
`branding.ini`; a key you leave out keeps the generic picture, and a path that
does not exist gives a CMake warning and the generic picture.

```ini
logo         = ../daytona-branding/logo.png
icon         = ../daytona-branding/icon-1024.png
vision_back  = ../daytona-branding/icon-vision-back.png
vision_front = ../daytona-branding/icon-vision-front.png
```

## 3. Build

Build as usual. CMake rereads `branding.ini` and the pictures it names whenever
they change; the configure log shows `Branding: <key> from <path>` for each one
in use. `-DDCUSA_BRANDING_FILE=<path>` uses a remap file somewhere else. For
the Mac app, run `tools/macapp/install_mac_app.sh` again.
