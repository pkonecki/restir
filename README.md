# ReSTIR GI

A C++ ReSTIR global illumination example using bgfx. It loads a glTF scene
using fastgltf (by default the bundled open-front Cornell box), then renders it
with a deferred PBR pipeline and ReSTIR GI whose reservoirs are reused
through a world-space hash grid, after Boissé, "World-Space Spatiotemporal
Reservoir Reuse for Ray-Traced Global Illumination" (SIGGRAPH Asia 2021).

The renderer needs compute shaders (Direct3D 11/12, Vulkan, OpenGL 4.3,
Metal or WebGPU); OpenGL ES is not supported.

## Requirements

- CMake 3.21 or newer
- A C++20 compiler
- Git

On Windows, bgfx requires MSVC 19.35 or newer (Visual Studio 2022 17.5+).

bgfx and fastgltf (v0.9.1) are fetched during CMake configuration.

## Build

```sh
cmake -S . -B build
cmake --build build --config Debug
```

Run the resulting `restir_gi` executable from the build directory. On
multi-configuration generators, the executable is placed in the `Debug`
subdirectory. The build compiles the bgfx shaders for the platform's supported
renderers and copies them and the model assets beside the executable.

### Models

The Cornell box is bundled. Crytek's Sponza can be added from the Khronos
glTF sample assets:

```sh
cmake -P cmake/fetch_sponza.cmake
```

This downloads the model and its 69 textures (about 55 MB) from a pinned
commit, checks their hashes, and places them in `assets/sponza`; files that
are already there and intact are skipped. Pass `-DSPONZA_TEXTURES=OFF` before
`-P` to fetch only the geometry (about 10 MB). Sponza is licensed under the
[CRYENGINE Limited License Agreement](https://www.cryengine.com/ce-terms),
so it is not part of this repository (`assets/sponza` is ignored by git) and
is not in the published web build. Re-run the build afterwards to copy it
beside the executable.

Press M to switch between the models that are present. To load another
model, pass its path; it comes first, ahead of the bundled ones:

```sh
restir_gi --model path/to/scene.gltf
```

The camera starts looking at the centre of the scene's bounds, from outside
the scene, except in Sponza, which is closed all around, where it starts
inside the courtyard. Distances in the GI pass scale with the scene's size. A
point light is placed in front of the scene's untextured emissive surfaces
(light panels), or near the top of the scene if it has none; for a closed
object, such as a helmet, that is inside it, so press L to bring the light to
the camera. Its intensity scales with the square of the scene's size. Primitives that are not triangle lists are skipped.

Materials use their base colour, metallic and roughness factors, multiplied
by their base colour and metallic-roughness textures and the vertex colours
(`COLOR_0`). Textures are read with `TEXCOORD_0`, from external files or
embedded, decoded with bimg, with mipmaps built at load time. Normal maps
use the model's tangents, or tangents derived from the texture coordinates
when it has none; they shade the direct light, while the GI passes keep the
surface's own normal, so that bumps do not stop neighbouring receivers from
sharing reservoirs. Occlusion textures dim the indirect light, as glTF
specifies. Emission (factor, `KHR_materials_emissive_strength` and texture)
is added to the image and, unless **Emissive lighting** (E) is off, lights
the scene through GI, from the emitters that are on screen. Alpha-masked
materials, such as Sponza's plants, are cut out in both the G-buffer and the
shadow map, and double-sided ones are lit from both sides. Texture
transforms, texture coordinate sets other than the first, and glTF samplers
(textures always repeat) are not used. A texture
that cannot be read is skipped with a warning; if it was a metallic-roughness
texture, its material is treated as non-metallic, since models with one
usually leave the metallic factor at its default of 1.

A few more command line options help with repeatable comparisons:

```sh
restir_gi --camera 0,1,4.5,0,0.1 --no-ui --screenshot out --screenshot-frame 120
```

`--camera x,y,z,yaw,pitch` starts from that view, `--no-ui` hides the
settings window (F2 toggles it), and `--screenshot` (bgfx's own option)
saves `out.png` after the given frame and exits.

The camera flies freely. Hold the left mouse button and drag to look around;
use W, A, S and D to move forward, left, back and right, and Shift and Ctrl
to move up and down. The speed follows the scene's size, and the mouse wheel
scales it. Surfaces seen from behind are not drawn, so the walls nearest the
camera disappear and the interior stays visible from any side.

### Settings window

Every toggle and setting is in the **ReSTIR GI** window (Dear ImGui, as
bundled with bgfx's example code); new ones belong there too. It has these
sections:

- **Scene**: the model (M cycles through them) and the camera speed.
  Below them:
  - **Views**: saved camera views for the current model (**Save this view**;
    keys 1 to 9 recall them), kept in `camera_bookmarks.txt` in the working
    directory. The text field shows the current view as
    `x,y,z,yaw,pitch`, the form `--camera` takes.
  - **Screenshot** (P): saves a PNG in `screenshots/`, named after the time,
    without the settings window unless **Hide this window** is unchecked. In
    the web build the browser downloads it.
- **Light**: the point light's position (drag the fields, or press L to move
  it to the camera), colour and intensity (relative to the default, which
  scales with the scene size), and an orbit that circles it around the
  scene's vertical axis at a chosen radius and speed. ReSTIR re-shades every
  reservoir's sample each frame, so the bounced light follows a moving light,
  while the denoised history trails it by a few frames. Loading a model, or
  **Reset light**, puts it back at the scene's default position.
- **Rendering**: multiple bounces (B), shadows (H), the denoiser (N),
  emissive lighting (E), and whether reservoirs are reused through the world-space hash grid or from
  screen-space neighbourhoods (G).
  The view (V cycles through them) shows the final image or a debug view:
  direct light only, indirect light only, or the GI irradiance before the
  albedo is multiplied in (tone mapped like the final image); or, as plain
  colours, the G-buffer's base colour, normals, roughness and metallic,
  emission and ambient occlusion, each
  pixel's reservoir M and W and GI history length (heat maps), and the
  world-space hash grid's cells. The tooltip says how to read each one.
  Below them, the exposure: the tone mapper (T cycles through AgX, the
  default, fitted ACES, Khronos PBR Neutral, Reinhard and a plain clamp), the
  exposure in stops, and auto exposure, which brings the log-average
  luminance of the image to middle grey, easing in at the chosen speed (the
  exposure slider then compensates on top of it).
- **Performance**: frames per second with a frame time plot, and the GPU time
  of each group of passes (shadow map, G-buffer, hash grid, ReSTIR temporal
  and spatial, denoiser, composite, tone mapping), measured with bgfx's per-view profiler.
  They need GPU timer queries; WebGPU offers them only in browsers that expose
  `timestamp-query`. Below them are the levers that trade quality for speed:

  | Setting | Default | Effect |
  | --- | --- | --- |
  | VSync | on | Off to measure how fast frames can go (also F7). |
  | GI resolution | half | Size of every ReSTIR and denoiser target: full, half or quarter of the window. The GI cost follows the pixel count. |
  | Shadow map face | 512 | Texels per side of each of the six cube faces, 256 to 2048. |
  | Denoiser iterations | 5 | A-trous passes, 1 to 5; each doubles the filter radius. |
  | Rays per pixel | 2 | Fresh candidates traced each frame in the temporal pass. |
  | Temporal taps | 2 | Reservoirs reused from last frame. |
  | Spatial taps | 4 | Reservoirs reused from this frame, each with a visibility ray. |
  | Ray march steps | 32 | Depth buffer steps per ray. Rays reach as far with fewer, coarser steps, but miss thin geometry. |

  **Reset to defaults** restores them.

## Web build (WebGPU)

The example also builds to WebAssembly with Emscripten, rendering through
bgfx's WebGPU renderer, so it needs a browser with WebGPU support.

shaderc cannot run as WebAssembly, so build it natively first. The native
build above already does; to build only shaderc:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target shaderc
```

Then configure the web build with the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
active, pointing it at that shaderc:

```sh
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release \
    -DRESTIR_HOST_SHADERC=$PWD/build/_deps/bgfx.cmake-build/cmake/bgfx/shaderc
cmake --build build-web
```

This writes `restir_gi.html`, `.js`, `.wasm` and `.data` to `build-web`; the
`.data` file packs the WGSL shaders and the bundled model. Browsers expose
WebGPU only to pages served over HTTPS or from localhost, so serve the
directory rather than opening the file:

```sh
python3 -m http.server -d build-web 8080
```

and open <http://localhost:8080/restir_gi.html>. The web build starts with
the bundled Cornell box, since `--model` has no command line to come from. A
local web build also packs Sponza if it has been fetched, and M switches to it.

`.github/workflows/pages.yml` runs these steps on every push and pull
request, and deploys pushes to `main` to GitHub Pages. To enable the
deployment, set **Settings → Pages → Build and deployment → Source** to
**GitHub Actions**.

Notes on the web build:

- It enables only the WebGPU renderer; there is no WebGL fallback.
- It links Asyncify so bgfx can wait for the browser's asynchronous
  WebGPU adapter and device requests.
- `web/webgpu_workarounds.js` works around Emscripten's emdawnwebgpu port
  passing an unset timestamp-query index to the browser as `0xFFFFFFFF`,
  which made every command encoder carrying bgfx's GPU timer invalid.
- Use a Release build: in Debug, bgfx's HTML5 entry asserts that its
  fullscreen request at start-up succeeds, which it cannot without a user
  gesture, so the page stops before rendering.

## Denoiser

The GI result is denoised in the style of SVGF (Schied et al. 2017), at GI
resolution, in `shaders/fs_svgf_variance.sc` and `shaders/fs_svgf_atrous.sc`:

1. The GI pass stores irradiance (its estimate divided by the diffuse albedo,
   which the composite pass multiplies back in), blended over up to 16 frames.
   Last frame's value is read from the four history texels around the
   reprojected position, keeping only those on the same surface, along with
   running luminance moments and a history length.
2. A variance pass turns the moments into a per-pixel noise estimate. Where
   the history is short, after disocclusion or camera motion, it estimates the
   noise from nearby pixels on the same surface instead, and boosts it.
3. Five a-trous wavelet iterations, with taps 1, 2, 4, 8 and 16 texels apart,
   average each pixel with neighbours on the same plane, with a similar normal,
   and with a luminance within what the noise explains. Noisy pixels are
   blurred hard, converged ones barely.

The output of the first iteration becomes the next frame's history, which
multiple bounces read as well.

## Shadows

The point light casts shadows from an omnidirectional shadow map: the scene
is rendered from the light once per cube face, into a 3x2 atlas of a 2D
texture that stores each surface's distance from the light. All faces are
drawn, including the ones hidden from the camera. The direct lighting filters
16 shadow samples for soft edges; the light at the surfaces rays hit for
bounced light uses one sample, which the accumulation over frames smooths.
The shared lookup is in `shaders/shadow.sh`.

## How the GI pass works

GI runs at half resolution by default. Each pixel's receiver (the surface it
sees) keeps one reservoir, and reservoirs are reused between receivers that
are close in the scene, found through a world-space hash grid as in Boissé
2021, rather than between pixels that are close on screen. Each frame (the counts below
are the defaults; the settings window changes them):

1. **Hash grid** (`shaders/cs_grid_clear.sc`, `shaders/cs_grid_insert.sc`,
   `shaders/hashgrid.sh`): two compute passes clear this frame's grid and
   list every GI pixel in the cell of its receiver. The cell size grows with
   the distance to the camera, so a cell covers about six GI pixels anywhere
   on screen. It is the smallest size times a power of two, and the size and
   the receiver's dominant normal axis are part of the cell key. Cells live
   in an open-addressing table of 65,536 slots, found with one hash and told
   apart with a second, 32-bit checksum hash, and each lists up to 32 pixels.
   Positions are jittered within the tangent plane by up to half a cell, both
   on insertion and on every lookup, so cell boundaries do not show up as
   blocks.
2. **Temporal pass** (`shaders/fs_restir_temporal.sc`): traces two
   cosine-distributed rays through the depth buffer and resamples the hits
   into a reservoir. It then combines that reservoir, using pairwise MIS and
   the reconnection Jacobian, with two reservoirs picked at random from last
   frame's grid cell around the receiver (M capped at 20). This needs no
   reprojection: a surface seen anywhere on screen last frame contributes if
   it lies in this receiver's cell. A sample taken over from another receiver
   is tested for visibility before the result is stored as this frame's
   reservoir.
3. **Spatial pass** (`shaders/fs_restir_spatial.sc`): combines the pixel's
   reservoir with four others picked from this frame's grid cell, again with
   pairwise MIS. The MIS weights check whether each neighbour could have
   found this pixel's sample, with one screen-space visibility ray per
   neighbour, so occluders next to a pixel do not darken it. The pass then
   tests the chosen sample for visibility, shades it, and hands the result to
   the denoiser (below). The spatially combined reservoir is not stored.

The reservoirs from step 2, with each receiver's position and normal (which
the MIS weights need), are the history that next frame's grid lookups read.
Press G to reuse screen-space neighbours instead: the pixel's reprojected
reservoir in step 2, and pixels within 12 texels in step 3. With the
denoiser off, the world-space grid is visibly less noisy under camera motion.

Differences from the paper: it resamples ray-traced path vertices over
several spatial passes, while this example traces rays through the depth
buffer and runs a single spatial pass. Cells keep the first 32 pixels to
arrive rather than a uniform choice, which does not bias anything, since which
reservoirs a cell lists does not depend on what they hold. The shared helpers
and the pairwise MIS are in `shaders/restir.sh`.

With multiple bounces enabled, a sample point is shaded with its direct light
plus the indirect light that left it in the previous frame, read back from
the denoised history. Each frame thus adds one bounce, and the lighting
converges to all bounces within a few frames. The composite pass upsamples the
result with a geometry-aware filter and adds direct PBR lighting, into a
16-bit float target. Auto exposure (`shaders/cs_exposure.sc`, one compute
group) averages the log luminance of 64x64 samples of it, and the tone
mapping pass (`shaders/fs_tonemap.sc`, operators in `shaders/tonemap.sh`)
applies the exposure and the tone mapper on the way to the window.

Known limits: rays only see the depth buffer, so off-screen, occluded and
back-facing geometry cannot contribute, which also caps the extra bounces. For
the same reason, a reservoir reused from the grid is dropped when its sample
point is no longer visible on screen. Bounced light lags a few frames behind
camera motion; shadow edges have a fixed softness rather than one that matches
the size of the light; and screen-space visibility and the clamped
reconnection Jacobian leave a bias of a few percent. This is a real-time
demonstration, not an unbiased multi-bounce path tracer.

## Source layout

- `src/main.cpp`: the application: window, keyboard shortcuts, the list of
  models, and the glue between the parts below.
- `src/scene.cpp`: the glTF loader (geometry, materials, texture decoding)
  and the framing of a scene (bounds, default light position).
- `src/renderer.cpp`: every GPU resource and pass, from the shadow map to the
  composite. `render` submits one frame.
- `src/camera.cpp`: the fly camera.
- `src/ui.cpp`: the settings window and the timings it shows.
- `src/settings.h`: the settings the window edits and the renderer reads.
- `shaders/`: the bgfx shaders, described in the sections above.

## Visual Studio Code

Install the Microsoft C/C++ extension. Use **Ctrl+Shift+B** to configure and
build the Debug target, or select **Debug ReSTIR GI** in the Run and Debug
view (F5) to build and launch it from the directory that holds its shaders
and assets.

## Credits

The Cornell box model is "Cornell Box- Original" by t-ly, downloaded from
[Sketchfab](https://sketchfab.com/3d-models/cornell-box-original-0d18de8d108c4c9cab1a4405698cc6b6)
and licensed under CC-BY-4.0. The full attribution and license details are in
[`assets/cornell_box/license.txt`](assets/cornell_box/license.txt).

This work is based on "Cornell Box- Original"
(https://sketchfab.com/3d-models/cornell-box-original-0d18de8d108c4c9cab1a4405698cc6b6)
by t-ly (https://sketchfab.com/t-ly) licensed under CC-BY-4.0
(http://creativecommons.org/licenses/by/4.0/).
