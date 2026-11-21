# voxup

Redraws small game sprites with AI image models and lets you pick the best one.

You load a 16 x 16 sprite and voxup asks one or more image models (run in [ComfyUI](https://github.com/comfyanonymous/ComfyUI),
hidden in the background) to redraw it 20 times with random seeds. It shrinks every drawing back into a sprite: 32 x 32,
16 x 16 or any size you type. You see them side by side, press **New 20 seeds** until one is right, click it and press
**Keep**: it is written into your game's sprite folder. Then load the next sprite.

## Examples
Free 16 x 16 sprites redrawn as 32 x 32 sprites by voxup with one model, FLUX.2 Klein 4B (distilled, 4 steps, about
5 s a picture). The colours are the original's own, with darker and lighter shades of them. For each item: the
original, the model's drawing, and the sprite voxup shrank it into, all shown at 4x. One seed of 8 was picked per item.

![Eight free sprites, the model's drawings and voxup's 32 x 32 sprites](docs/examples.png)

A round is many seeds side by side. Click the one you like; press **New seeds** for more:

![16 seeds of one sword](docs/round.png)

The window, after a round of 8 seeds of a key:

![The voxup window](docs/window.png)

The sprites in these pictures are from OpenGameArt and are CC0 (public domain):
[16x16 RPG items](https://opengameart.org/content/16x16-rpg-items) by Jetrel and
[16x16 RPG Icon Pack](https://opengameart.org/content/16x16-rpg-icon-pack) by stealthix. Thank you to both.

One Windows program in plain C: the window is [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) with its GDI
backend, pictures are read and written with [stb](https://github.com/nothings/stb). The models run either inside voxup
(the built-in engine, [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) on Vulkan, when built with
it) or in ComfyUI, driven over its local HTTP API. No Python of its own, no installer.

## Setting it up
1. Build it (below), or put `voxup.exe` in a folder with the `models` folder of this repo beside it.
2. Copy `voxup.ini.example` to `voxup.ini` beside `voxup.exe` and fill in your folders: the original sprites
   (`NNN.png`), ComfyUI's folder, ComfyUI's models folder, and the folder(s) Keep writes into.
3. Start `voxup.exe`.

## Using it
- **Item**: choose a sprite from the list, or step with `<` and `>`. Three pictures are shown: the original, your own
  enlarged one (`enlarged =`) and the item stood upright (`upright =`), when there are such files.
- **Draw from**: which of them the models are given (saved per item; the upright one when the item has it). A drawing
  made from the upright picture is shrunk into the upright picture's box. Each has its own description
  (`desc.txt`, `desc-upright.txt`).
- **What it is**: a description of the item; it goes into the prompt in place of `{d}`. Saved per item.
- **Engine**: **Built-in** draws the models that have `sd` lines inside voxup (no ComfyUI; the model stays loaded between
  rounds); **ComfyUI** draws them in ComfyUI. A model only one of them can draw always uses that one.
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
- **Prompts**: each model's prompt, editable, with a button back to the standard one. For a model that repaints the
  item's picture (Stable Diffusion): **How much may change** (5 to 100 %) and **Its LoRA** (0 to 200 %), saved per model.
- **Shrink settings** (right panel): every step between the model's drawing and the sprite, below. Changing a setting
  makes the shown round again from the stored drawings, without drawing again.

## The models
`models/models.txt` lists them. A model can have a ComfyUI API graph `models/ID.json` (written from ComfyUI's own
workflow templates; `"@IMAGE@"`, `"@PROMPT@"`, `"@SEED@"`, `"@PREFIX@"`, `"@STRENGTH@"` and `"@LORA@"` are filled in
for each picture), `sd` lines for the built-in engine (the files it loads and its settings, by stable-diffusion.cpp's
option names), or both. Adding a model is a block in models.txt (and a graph file); no code changes.

| id | model | steps | built-in | ComfyUI |
|---|---|---|---|---|
| klein | FLUX.2 Klein 4B (base) | 20 | yes | yes |
| kontext | FLUX.1 Kontext dev | 24 | yes | yes |
| klein4f | FLUX.2 Klein 4B (distilled) | 4 | yes | yes |
| klein9kv | FLUX.2 Klein 9B KV | 4 | | yes |
| qwen2511f | Qwen-Image-Edit 2511 + Lightning | 4 | | yes |
| qwen2511 | Qwen-Image-Edit 2511 | 40 | | yes |
| longcat | LongCat-Image-Edit | 50 | | yes |
| mageturbo | Mage-Flow-Edit turbo | 4 | | yes |
| boogu | Boogu-Image 0.1 edit | 25 | | yes |
| joyai | JoyAI-Image-Edit | 40 | | yes |
| omnigen2 | OmniGen2 | 20 | | yes |
| sdxlpix | Stable Diffusion XL 1.0 + Pixel Art XL LoRA (repaints the picture) | 30 | yes | yes |

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
- `voxup --selftest SHOT.bmp [ITEM] [--get ID] [--draw ID N ...] [--seeds A,B] [--set key=value] [--tab 1] [--from tool|upright|original] [--keep]`:
  the window off screen, never shown; it can get a model, draw, set shrink settings and press Keep (into
  `work-selftest\selftest-game`), then writes the window's picture.
- `voxup IN.raw OUT.raw [switches]`: the older voxel upscaler (`src/voxup.c`).

## Building
`build.cmd` with [LLVM clang](https://github.com/llvm/llvm-project) for the MinGW target on the PATH (or set `LLVM_ENV`
to a .cmd that puts it there). It builds `voxup.exe` beside it, drawing with ComfyUI.

### The built-in engine
`build.cmd sd` builds voxup with [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) (MIT) inside,
on Vulkan, so models with `sd` lines draw without ComfyUI. stable-diffusion.cpp is not in this repository; get it and
build it first with the same clang and the [Vulkan SDK](https://vulkan.lunarg.com/):

```
git clone --recursive https://github.com/leejet/stable-diffusion.cpp
cmake -S stable-diffusion.cpp -B sd-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang ^
  -DCMAKE_CXX_COMPILER=clang++ -DSD_VULKAN=ON -DGGML_OPENMP=OFF -DSD_WEBP=OFF -DSD_WEBM=OFF
cmake --build sd-build
set SDCPP=%CD%\stable-diffusion.cpp
set SDCPP_BUILD=%CD%\sd-build
build.cmd sd
```

(clang for MinGW also needs `--target=x86_64-w64-windows-gnu`, `-D_WIN32_WINNT=0x0A00` and `-static` at link time,
in `CMAKE_C_FLAGS`, `CMAKE_CXX_FLAGS` and `CMAKE_EXE_LINKER_FLAGS`.) The engine uses the same model files as ComfyUI.
On the graphics card it shares, voxup asks a ComfyUI it started to let go of its models before drawing built-in.

## Third-party code
`src/third_party`: Nuklear and its GDI backend, stb_image and stb_image_write, each public domain / MIT, at the commits
in `src/third_party/VERSIONS.txt`. stable-diffusion.cpp and its ggml (MIT) are linked in by `build.cmd sd`, not kept here. The resampling follows Pillow's and the model graphs follow ComfyUI's workflow
templates; their notices are in [THIRD_PARTY.md](THIRD_PARTY.md). No AI model is included.

## Credits
Most of voxup was written by Claude, Anthropic's AI, working on the design with the repository's owner.
