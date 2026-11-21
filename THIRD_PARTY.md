# Third-party code and notices

## Included as they are (src/third_party)
- **Nuklear** and its GDI backend (`nuklear.h`, `nuklear_gdi.h`), github.com/Immediate-Mode-UI/Nuklear: public domain
  or MIT, the author's choice; the licence text is at the end of `nuklear.h`.
- **stb_image** and **stb_image_write**, github.com/nothings/stb: public domain or MIT; the licence text is at the end
  of each file.

The commits used are in `src/third_party/VERSIONS.txt`.

## Followed, not copied
- **Pillow's resampling.** `src/pick.c` (`coeffs`, `resize_f`) and `src/shrink.c` (`coeffs`, `resize_k`) work out the
  filter weights the way Pillow's `Resample.c` does, so the results match Pillow's to the pixel. Pillow's notice:

  > The Python Imaging Library (PIL) is Copyright © 1997-2011 by Secret Labs AB, Copyright © 1995-2011 by Fredrik Lundh
  > and contributors. Pillow is the friendly PIL fork. It is Copyright © 2010 by Jeffrey 'Alex' Clark and contributors.
  > Like PIL, Pillow is licensed under the open source MIT-CMU License:
  >
  > By obtaining, using, and/or copying this software and/or its associated documentation, you agree that you have
  > read, understood, and will comply with the following terms and conditions:
  >
  > Permission to use, copy, modify and distribute this software and its documentation for any purpose and without fee
  > is hereby granted, provided that the above copyright notice appears in all copies, and that both that copyright
  > notice and this permission notice appear in supporting documentation, and that the name of Secret Labs AB or the
  > author not be used in advertising or publicity pertaining to distribution of the software without specific,
  > written prior permission.
  >
  > SECRET LABS AB AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED
  > WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL SECRET LABS AB OR THE AUTHOR BE LIABLE FOR ANY
  > SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
  > WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE
  > USE OR PERFORMANCE OF THIS SOFTWARE.

- **ComfyUI's workflow templates.** The graphs in `models/*.json` were written by hand after the models' settings in
  Comfy Org's workflow templates (github.com/Comfy-Org/workflow_templates). Their notice:

  > MIT License. Copyright (c) 2023-present Comfy Org
  >
  > Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
  > documentation files (the "Software"), to deal in the Software without restriction, including without limitation
  > the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
  > permit persons to whom the Software is furnished to do so, subject to the following conditions:
  >
  > The above copyright notice and this permission notice shall be included in all copies or substantial portions of
  > the Software.
  >
  > THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
  > WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
  > OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
  > OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

- **Methods from papers** (DPID, SAID, superpixels, dithering and the classic filters) are our own code written from
  the published descriptions; the papers are named in `src/shrink.c`.

## Not included
No AI model is in this repository. `models/models.txt` only links to each model's own page; downloading one with Get
means accepting that model's own licence.
