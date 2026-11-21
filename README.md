# voxup

Redraws small game sprites with AI image models and lets you pick the best one.

You load a 16 x 16 sprite and voxup asks one or more image models (run in [ComfyUI](https://github.com/comfyanonymous/ComfyUI),
hidden in the background) to redraw it 20 times with random seeds. It shrinks every drawing back into a sprite: 32 x 32,
16 x 16 or any size you type. You see them side by side, press **New 20 seeds** until one is right, click it and press
**Keep**: it is written into your game's sprite folder. Then load the next sprite.

One Windows program in plain C: the window is [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) with its GDI
backend, pictures are read and written with [stb](https://github.com/nothings/stb), ComfyUI is driven over its local
HTTP API. No Python of its own, no installer.

## Setting it up
1. Build it (below), or put `voxup.exe` in a folder with the `models` folder of this repo beside it.
2. Copy `voxup.ini.example` to `voxup.ini` beside `voxup.exe` and fill in your folders: the original sprites
   (`NNN.png`), ComfyUI's folder, ComfyUI's models folder, and the folder(s) Keep writes into.
3. Start `voxup.exe`.

## Using it
- **Item**: choose a sprite from the list, or step with `<` and `>`. The original is shown on the left.
- **What it is**: a description of the item; it goes into the prompt in place of `{d}`. Saved per item.
- **Models**: how many pictures each model draws in a round (0 to 20, at most 40 a round). **Only** = that model alone.
  A model not on your computer yet shows **Get N GB**: its files are downloaded in the background straight into
  ComfyUI's models folder (each file as `.part` until it is complete and the right size; a stopped download goes on
  from where it stopped; files two models share are got once; free disk space is checked first).
- **Draw / New seeds**: ComfyUI is started hidden the first time (about 30 s). Each picture appears as soon as it is
  ready. **Stop** stops after the picture being drawn.
- **< Back / Next >**: earlier rounds; they are kept on disk.
- **Click a picture**: it is shown large beside the original and the model's own drawing. **Keep this one** writes it
  into every `game =` folder of voxup.ini (a picture already there is backed up first, under `work\backup`); one bigger
  than `game_max` goes to `work\export`. Every keep is a line in `work\keep.txt`.
- **Prompts**: each model's prompt, editable, with a button back to the standard one.
- **Shrink settings** (right panel): every step between the model's drawing and the sprite, below. Changing a setting
  makes the shown round again from the stored drawings, without drawing again.

## The models
`models/models.txt` lists them; each one has a ComfyUI API graph `models/ID.json` written from ComfyUI's own workflow
templates, with `"@IMAGE@"`, `"@PROMPT@"`, `"@SEED@"` and `"@PREFIX@"` filled in for each picture. Adding a model is a
block in models.txt and a graph file; no code changes.

| id | model | steps |
|---|---|---|
| klein | FLUX.2 Klein 4B (base) | 20 |
| kontext | FLUX.1 Kontext dev | 24 |
| klein4f | FLUX.2 Klein 4B (distilled) | 4 |
| klein9kv | FLUX.2 Klein 9B KV | 4 |
| qwen2511f | Qwen-Image-Edit 2511 + Lightning | 4 |
| qwen2511 | Qwen-Image-Edit 2511 | 40 |
| longcat | LongCat-Image-Edit | 50 |
| mageturbo | Mage-Flow-Edit turbo | 4 |
| boogu | Boogu-Image 0.1 edit | 25 |
| joyai | JoyAI-Image-Edit | 40 |
| omnigen2 | OmniGen2 | 20 |

Each model's own licence applies to its files (see their Hugging Face pages, linked in models.txt).

## Shrink settings
In pipeline order (`src/shrink.c`, sources in its header):
- **Size**: 16 x 16, 32 x 32, or any width and height (1 to 128). **Fit**: the original's own box (scaled), the whole
  sprite, or stretched; a margin kept free.
- **Cutting out the background**: distance from the corners' colour; plain magenta too; the cover a pixel needs.
- **Before shrinking**: contrast, brightness, saturation; median or bilateral smoothing; sharpening at the size of a
  sprite pixel.
- **Method**: Lanczos (standard), Nearest, Box, Bilinear, Bicubic, Mitchell-Netravali, Gaussian, Median, Majority (the
  commonest colour of each pixel's area), **DPID** (Weber et al., *Rapid, Detail-Preserving Image Downscaling*,
  SIGGRAPH Asia 2016), **SAID** (Arjun, Mitra, Ghosh, *Structure Aware Image Downscaling*, arXiv 2510.22551, 2025; the
  paper gives no values, ours are our own reading), **Superpixels** (after Gerstner et al., *Pixelated Image
  Abstraction*, NPAR 2012). Then pinholes filled.
- **Colours**: the original's own, the original's with darker and lighter shades, the model's own (background pink
  still snapped), the model's own as drawn, or N colours found (k-means or median cut); nearest by RGB, Lab or weighted
  RGB; Floyd-Steinberg, Atkinson or ordered (Bayer) dithering.
- **The model's black line**: take it off (1 to 4 layers) or leave it.
- **Clean-up**: lone pixels take their neighbours' colour; doubled corners in black lines removed.
- **Outline**: none, black, black with corners, or selective (a darker shade of the colour beside it); 1 to 4 pixels
  thick (1 standard). Without an outline the edge can be rough (as shrunk) or smooth (steps evened out with whole
  pixels, no see-through pixels).

## Command line
- `voxup --models`: which models are ready and what each still needs.
- `voxup --to32 DRAWING.png ITEM OUT.png [--own]`: one drawing to the standard 32 x 32 sprite.
- `voxup --shrink DRAWING.png ITEM OUT.png [key=value ...]`: one drawing through any shrink settings (the names are
  those in `work\pick.ini`'s `shrink.` lines, e.g. `method=9 outline=3 othick=2 size=2 cw=24 ch=40`).
- `voxup --shrinkcheck DIR`: the pipeline with its standard settings against `--to32` on every drawing in
  `DIR\NNN\*-sSEED.png` (they must be equal).
- `voxup --selftest SHOT.bmp [ITEM] [--get ID] [--draw ID N ...] [--seeds A,B] [--set key=value] [--tab 1] [--keep]`:
  the window off screen, never shown; it can get a model, draw, set shrink settings and press Keep (into
  `work-selftest\selftest-game`), then writes the window's picture.
- `voxup IN.raw OUT.raw [switches]`: the older voxel upscaler (`src/voxup.c`).

## Building
`build.cmd` with [LLVM clang](https://github.com/llvm/llvm-project) for the MinGW target on the PATH (or set `LLVM_ENV`
to a .cmd that puts it there). It builds `voxup.exe` beside it.

## Third-party code
`src/third_party`: Nuklear and its GDI backend, stb_image and stb_image_write, each public domain / MIT, at the commits
in `src/third_party/VERSIONS.txt`. The resampling follows Pillow's and the model graphs follow ComfyUI's workflow
templates; their notices are in [THIRD_PARTY.md](THIRD_PARTY.md). No AI model is included.

## Credits
Most of voxup was written by Claude, Anthropic's AI, working on the design with the repository's owner.
