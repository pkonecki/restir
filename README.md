# ReSTIR GI

A C++ ReSTIR global illumination example using bgfx. It loads a glTF scene
using fastgltf (by default the bundled open-front Cornell box), then renders it
with a deferred PBR pipeline and ReSTIR GI whose reservoirs are reused
through a world-space hash grid, after Boissé, "World-Space Spatiotemporal
Reservoir Reuse for Ray-Traced Global Illumination" (SIGGRAPH Asia 2021).
Rays the depth buffer cannot answer are traced through a BVH of the scene;
emissive surfaces and extra point lights are lit with ReSTIR DI; a sky,
glossy reflections, TAA, exposure and tone mapping complete the image; and a
reference renderer measures the bias the real-time shortcuts leave.

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

A few more command line options help with repeatable comparisons (and
`--measure`, under **Reference** below):

```sh
restir_gi --camera 0,1,4.5,0,0.1 --no-ui --screenshot out --screenshot-frame 120
```

`--camera x,y,z,yaw,pitch` starts from that view, `--settings` applies a
share string (the view and every setting, as the settings window shows it
under **Views**), `--no-ui` hides the settings window (F2 toggles it), and `--screenshot` (bgfx's own option)
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
  **Reset light**, puts it back at the scene's default position. Below it,
  **Extra lights** scatters up to 1,024 coloured point lights through the
  scene (seeded; **Scatter again** moves them), lit with ReSTIR DI (below).
- **Sky** (K): a gradient from the ground through the horizon to the zenith
  colour, with an intensity. It shows behind the scene and lights it
  through the GI rays that the BVH confirms leave the scene, so it needs the
  BVH ray modes (see "Off-screen geometry"). A reservoir then holds a
  direction instead of a point, compared between receivers in solid angle;
  its visibility from another receiver is always tested through the BVH,
  since the depth buffer cannot tell that nothing lies off screen. It
  starts on for Sponza and models passed with `--model`, and off for the
  Cornell box, whose open front would let it flood the room.
- **Rendering**: multiple bounces (B), shadows (H), the denoiser (N), TAA,
  specular GI, emissive lighting (E), how emitters and extra lights are lit
  (ReSTIR DI, or through GI only), how rays are traced (R: depth buffer, then
  BVH, or either alone), whether reservoirs are reused through the
  world-space hash grid or from screen-space neighbourhoods (G), and the two
  optional clamps (see "Bias").
  The view (V cycles through them) shows the final image or a debug view:
  direct light only, indirect light only, the GI irradiance before the
  albedo is multiplied in, or indirect specular only (tone mapped like the
  final image); or, as plain colours, the G-buffer's base colour, normals,
  roughness and metallic, emission and ambient occlusion, each pixel's
  reservoir M and W and GI history length (heat maps), the world-space hash
  grid's cells, and where each reservoir's sample came from. The tooltip
  says how to read each one.
  **TAA** (temporal anti-aliasing) renders each frame with a different
  sub-pixel offset and blends the frames: the history is reprojected from
  depth with last frame's unjittered camera, read with a Catmull-Rom filter,
  and clipped to the current pixel's neighbourhood so moving the camera does
  not leave ghosts (`shaders/fs_taa.sc`). It runs on the HDR image before
  the exposure, and is skipped by the data debug views.
  Below them, the exposure: the tone mapper (T cycles through AgX, the
  default, fitted ACES, Khronos PBR Neutral, Reinhard and a plain clamp), the
  exposure in stops, and auto exposure, which brings the log-average
  luminance of the image to middle grey, easing in at the chosen speed (the
  exposure slider then compensates on top of it).
- **Reference** (collapsed): **Accumulate reference** averages frames of
  the current view with every biased shortcut off (no reuse in GI or DI, no
  denoiser, no TAA, no clamps; the camera keeps jittering, so the result is
  anti-aliased), which converges to the unbiased image of the same rays.
  Moving the camera or the light starts it again. Once it is stopped, the
  window shows the live image's relative MSE and brightness bias against it
  (measured every 30 frames by `shaders/cs_error.sc` and read back), and the
  display can switch to the reference or to a heat map of the per-pixel
  error (green none, blue darker, red brighter). `--measure N` does the same
  from the command line: it accumulates N frames, lets the live settings
  settle, prints the two numbers and exits.
- **Performance**: frames per second with a frame time plot, and the GPU time
  of each group of passes (shadow map, G-buffer, ReSTIR DI temporal and
  spatial, hash grid, ReSTIR GI temporal and spatial, specular GI, denoiser,
  composite, tone mapping), measured with bgfx's per-view profiler.
  They need GPU timer queries; WebGPU offers them only in browsers that expose
  `timestamp-query`. Below them are the levers that trade quality for speed:

  | Setting | Default | Effect |
  | --- | --- | --- |
  | VSync | on | Off to measure how fast frames can go (also F7). |
  | GI resolution | half | Size of every ReSTIR and denoiser target: full, half or quarter of the window. The GI cost follows the pixel count. |
  | Shadow map face | 512 | Texels per side of each of the six cube faces, 256 to 2048. |
  | Denoiser iterations | 5 | A-trous passes, 1 to 5; each doubles the filter radius. |
  | Rays per pixel | 2 | Fresh candidates traced each frame in the temporal pass. |
  | Temporal taps | 2 | Reservoirs reused from last frame, 0 to 4. |
  | Spatial taps | 4 | Reservoirs reused from this frame, each with a visibility ray, 0 to 8. Both at 0: no reuse, the unbiased reference (see "Bias"). |
  | DI candidates | 8 | Fresh light samples per pixel per frame in ReSTIR DI. |
  | DI spatial taps | 3 | Neighbouring DI reservoirs reused, each with a shadow ray. |
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
the bundled Cornell box. A local web build also packs Sponza if it has been
fetched, and M switches to it. Other models load in two ways:

- **Drop** a `.glb` file, or a `.gltf` with its buffers and textures (the
  files, or the folder holding them), onto the page.
- **`?model=URL`** in the address: the page downloads a `.glb`, or a
  `.gltf` and the buffers and images it refers to by relative URI, before
  starting. The server has to allow the page to fetch it (same origin, or
  CORS).

The page's address keeps the view and every setting in its hash (the share
string the settings window shows under **Views**, which `--settings` also
takes natively), updated as they change: copy the address to share exactly
what you see.

`.github/workflows/build.yml` builds the example natively on every push and
pull request: on Linux (GLSL and SPIR-V shaders), where it also renders the
Cornell box for 30 frames under Xvfb with Mesa's software rasterizer and
checks the frame is not blank (the frame is kept as an artifact); on
Windows with MSVC (HLSL through fxc and dxc); and, when run by hand from
the Actions tab, on macOS (Metal).

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
   apart with a second, 32-bit checksum hash, and each lists up to 32 pixels
   (checksums, counts and entries share one buffer).
   Positions are jittered within the tangent plane by up to half a cell, both
   on insertion and on every lookup, so cell boundaries do not show up as
   blocks.
2. **Temporal pass** (`shaders/cs_restir_temporal.sc`): traces two
   cosine-distributed rays through the depth buffer, and the rays it cannot
   answer through the scene's BVH (below), and resamples the hits into a
   reservoir. It then combines that reservoir, using pairwise MIS and
   the reconnection Jacobian, with two reservoirs picked at random from last
   frame's grid cell around the receiver (M capped at 20; see "Bias"). This needs no
   reprojection: a surface seen anywhere on screen last frame contributes if
   it lies in this receiver's cell. A sample taken over from another receiver
   is tested for visibility before the result is stored as this frame's
   reservoir.
3. **Spatial pass** (`shaders/cs_restir_spatial.sc`): combines the pixel's
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

### Glossy reflections: specular GI

The ReSTIR passes resample for the diffuse part of the light. Glossy
reflections get a pass of their own at GI resolution
(`shaders/fs_specular.sc`): one ray per pixel sampled from the GGX
distribution of visible normals (Heitz 2018), traced like the GI rays
(depth buffer, then BVH, then sky). Where it hits a visible surface, it
takes last frame's HDR image there, so reflections show everything,
reflections included, one frame late; off screen it shades the hit
triangle as GI does. The estimate is divided by the split-sum specular
albedo (Karis's analytic fit) and accumulated over 4 frames on a mirror to
32 on a rough surface, and the composite multiplies the albedo back in.
Metals, which reflect no diffuse light, get their indirect light this way
(see the Khronos DamagedHelmet sample). **Specular GI** (Rendering section)
turns it off, and **Indirect specular only** shows it alone.

Limits: one sample per GI pixel without a spatial filter, so rough
surfaces stay somewhat noisy (TAA smooths it); reflections of off-screen
geometry have no texture detail beyond each triangle's colour; and the
reflected image is last frame's, as seen from the camera rather than from
the reflection.

### Direct light from many lights: ReSTIR DI

The main point light keeps its shadow map. Everything else that emits, the
scene's emissive triangles and the extra point lights, goes into a light
list (`src/lights.cpp`), sampled with ReSTIR DI (Bitterli et al. 2020) at
full resolution (`shaders/di.sh`):

1. **Temporal pass** (`shaders/cs_di_temporal.sc`): 8 candidates pick a
   light in proportion to its power and a point on it (uniform over a
   triangle), resampled by their unshadowed contribution (diffuse plus GGX
   specular at the pixel). The result is combined with the pixel's
   reprojected reservoir from last frame with the generalized balance
   heuristic of the two, whose targets are evaluated at each receiver (last
   frame's from its stored position, normal and camera, with visibility).
   The selected sample is tested with a shadow ray through the BVH and
   stored.
2. **Spatial pass** (`shaders/cs_di_spatial.sc`): combines the pixel's
   reservoir with three from a 16-pixel disk that see the same surface,
   with pairwise MIS on the full target functions (a light sample is the
   same point for every pixel, so the shift's Jacobian is 1). It shades the
   result into diffuse and specular radiance for the composite.

Shadows are exact (BVH rays) and soft for area lights. The DI diffuse light
is also what the GI sample points reflect, so emitters and extra lights
bounce through GI without being counted twice (the GI's own emission term
is turned off). **Direct lighting** (Rendering section) switches back to
the old behaviour, where emitters only light the scene through the GI rays
that happen to hit them. Emissive triangles emit from both sides, since
models wind them inconsistently. The bundled Cornell box's light panel has
an emissive factor of 1, which is dim next to the point light that stands in
for it: turn the point light down to see the panel's light alone.

### Bias

The reuse follows GRIS (Lin et al. 2022, "Generalized Resampled Importance
Sampling"): pairwise MIS weights, in the area measure at the sample point,
between the canonical reservoir and each neighbour, with the reconnection
Jacobian applied to every shifted sample (`shaders/restir.sh`). Under GRIS:

- **Capping M at 20 adds no bias.** Any confidence weights are allowed as
  long as the MIS weights sum to one; the cap only trades how much the
  history counts against how fast it adapts.
- **The Jacobian uses the same distances as the MIS weights.** It used to
  clamp both distances to a minimum, while the geometry terms of the MIS
  weights used the true ones. A sample found very close to its receiver
  (along corners and edges) then got a Jacobian up to a hundred times too
  large when moved to a farther receiver, while its MIS weight still handed
  it the whole pair. Temporal reuse through the hash grid gained light
  along corners that way, multiple bounces fed back on it, and it was one of
  the two causes of light slowly spreading from corners over hundreds of
  frames, visible first in the reservoir weights (panning away reset it, as
  the reservoirs were dropped). TAA's camera jitter made it much more
  likely, by moving the receivers over the whole pixel, arbitrarily close to
  the walls. The other cause is handled by the boiling filter (below).
- **The Jacobian stays clamped** at 10 (**Clamp Jacobian**, Rendering
  section), which bounds how much weight one shift can add, at the cost of a
  slight darkening where samples are reused across large changes in distance
  or angle.
- **The boiling filter** (**Boiling filter**, Rendering section, as in
  RTXDI's ReSTIR GI) limits each temporal reservoir's estimate to 4 times
  what its pixel showed last frame (or, where it has no history, the mean
  estimate of its 8x8 group of pixels), or what its fresh candidates show
  now, whichever is larger. Even with exact weights, contribution weights are
  heavy-tailed, and with multiple bounces the integrand is made of last
  frame's estimates: a reservoir with an outlying weight brightens the
  history its own sample reads its light from, and temporal reuse through
  the hash grid keeps and spreads it. In the Cornell box with TAA on and
  the Jacobian fixed, the light still spread from corners in about half of
  the long runs without the filter (within 600 to 1,100 frames), and in most
  with the Jacobian clamp off as well; with the filter, none of the runs did
  over 2,000 frames, the clamp on or off. It only acts on outliers: the
  defaults measure -0.42% against the reference with it and -0.05% without,
  about the noise of the measurement, and a relative MSE of 0.00120.
- **Clamp outliers** caps each pixel's estimate at 8 before accumulation, as
  before. It is biased but cheap insurance against fireflies; in the Cornell
  box it changes the mean by 0.05%.
- Fresh rays leave from slightly above the receiver. Their density is
  converted to the solid angle at the receiver itself (the measure of the
  target function), through the density per unit area at the hit point, so
  with the cosine there as seen from each point: for a hit next to a
  receiver close to a wall, the two cosines differ a lot.

Setting both tap counts to 0 turns reuse off: each pixel then uses its fresh
candidates alone, which is plain, unbiased Monte Carlo (given the same rays)
and makes a reference to measure the reuse against. In the Cornell box, with
the light and camera at their defaults, BVH rays (so that visibility is
exact), multiple bounces, the denoiser and the Jacobian clamp off, and the
mean of the indirect light over 25 frames (frames 60 to 420), measured
before the Jacobian fix above:

| Reuse | vs. no reuse |
| --- | --- |
| Spatial only (grid, 4 taps) | +0.03% |
| Temporal only, screen space (own reservoir, reprojected) | +0.01% |
| Screen-space temporal, then screen-space spatial | +0.05% |
| Temporal only, grid, 1 / 2 / 4 taps | +0.28% / +0.90% / +1.57% |
| Defaults (grid, 2 temporal and 4 spatial taps) | +0.86% |

The gain of temporal reuse through the hash grid, which the error view put
in corners and contact shadows, was the Jacobian mismatch. With the fix,
temporal reuse alone through the grid (2 or 4 taps, same settings as above)
measures -0.10% and +0.36% against a 256-frame reference (`--measure 256`,
mean over the frame), within the noise of the measurement, and the defaults
(before the boiling filter) -0.05% instead of +0.21%, with the relative MSE
down from 0.00134 to 0.00129.

### Off-screen geometry: the BVH

A ray marched through the depth buffer only finds surfaces on screen. Rays
that leave the screen, or pass behind what it shows, find nothing there, so
by default they are traced again through a bounding volume hierarchy of every
triangle in the scene. It is built on the CPU when a model loads (binned SAH,
up to four triangles per leaf; Sponza's 262,000 triangles take under 0.2 s
and 32 MB) and uploaded as one GPU buffer that also holds each triangle's
diffuse albedo and emission (material factors and vertex colours, times its
textures at the centre of its texture coordinates, read from the mip level
that matches the triangle's size in the texture) (`src/bvh.cpp`; traversal
in `shaders/bvh.sh`). The nodes are stored in depth-first order, with a skip
link to the node after each subtree, so the pixel shaders traverse without a
stack: a per-thread stack array cannot stay in registers there and spilled to
scratch memory (Radeon GPU Profiler). The ReSTIR compute passes visit the
nearer child first for closest-hit rays, with the stack in shared memory.

A reservoir stores the triangle its sample lies on, as well as the side its
ray came from. A sample point that is a visible surface is shaded from the
G-buffer as before; one that is off screen or hidden is shaded from its
triangle: its albedo, its plane normal, the point light through the
cube shadow map (which covers the whole scene), and its emission. Such
samples therefore survive camera motion that hides them, where samples found
in the depth buffer are dropped. The **Rays** setting (R) chooses:

- **Depth buffer only**: as before.
- **Depth buffer, then BVH** (default): the BVH for the rays the depth
  buffer cannot answer. Visibility tests between receivers and reused samples
  stay in screen space.
- **BVH only**: every ray and every visibility test goes through the BVH,
  which removes the screen-space visibility bias at a higher cost.

The **Sample source** debug view shows, per pixel, whether the reservoir's
sample came from the depth buffer or the BVH, and whether it is visible. On
Mesa's software rasterizer, the hybrid mode made the temporal pass about 2.5
times as expensive in Sponza, and BVH only also doubled the spatial pass;
GPUs will differ.

Known limits: off-screen sample points have no texture detail within a
triangle (one colour per triangle), ignore alpha masks (foliage is opaque to BVH rays),
and get no bounce light of their own, since that comes from the on-screen
history; so the extra bounces still come from what is on screen. Samples
found in the depth buffer are dropped when they leave the screen, as the
G-buffer does not record which triangle they lie on. Bounced light lags a few frames behind
camera motion; shadow edges have a fixed softness rather than one that matches
the size of the light; and screen-space visibility and the clamped
reconnection Jacobian leave a bias of a few percent. This is a real-time
demonstration, not an unbiased multi-bounce path tracer.

## GPU performance

The four ReSTIR passes (GI and DI, temporal and spatial) are compute
shaders, in groups of 8x8 pixels (`shaders/compute.sh`):

- **BVH rays**: closest-hit rays keep their traversal stack in shared
  memory (40 entries per thread), visibility rays need no stack (above).
- **The queue of BVH rays** (GI temporal pass): the fresh rays the depth
  buffer cannot answer are queued in shared memory and traced by
  consecutive threads, so a wave traces only when it has rays to trace,
  instead of every wave waiting for the few lanes that need the BVH.
- **The boiling filter** falls back to the mean estimate of the group's
  pixels where a pixel has no history (see "Bias").
- **No `&&`, `||` or `?:` around expensive calls**: bgfx compiles the
  shaders as HLSL for Vulkan, Direct3D, Metal and WebGPU, and HLSL (before
  HLSL 2021) evaluates both sides of those operators. Visibility rays that
  should only run when a neighbour's sample was selected, for instance, ran
  for every pixel.

**Wave intrinsics** (Rendering section, Vulkan only) switches the four
passes to variants compiled with wave (subgroup) operations, as SPIR-V 1.3,
to measure what a per-platform path would gain (compare the pass timings
in the Performance section with it on and off):

- The queue takes one shared-memory atomic per wave (ballot and prefix
  count) instead of one per ray.
- The boiling filter's tile mean is a wave sum, then a sum over the waves,
  instead of a tree reduction with six barriers.
- Visibility rays traverse the BVH in step across the wave: each step, the
  wave visits the smallest node any of its lanes still needs, read once at
  a wave-uniform index for all of them. That pays off when the rays of a
  wave are coherent (shadow rays to the same light), and costs node visits
  when they are not.

The variants use the HLSL Shader Model 6 wave functions, which bgfx's shader
compiler turns into Vulkan subgroup operations; the same code would compile
for Direct3D 12 with dxc, and Metal and WebGPU have equivalents (the latter
as an optional feature). They assume that a group's waves are made of
consecutive threads, as on every GPU we know of.

## Source layout

- `src/main.cpp`: the application: window, keyboard shortcuts, the list of
  models, and the glue between the parts below.
- `src/scene.cpp`: the glTF loader (geometry, materials, texture decoding)
  and the framing of a scene (bounds, default light position).
- `src/renderer.cpp`: every GPU resource and pass, from the shadow map to the
  composite. `render` submits one frame.
- `src/bvh.cpp`: the BVH for the rays the depth buffer cannot answer, with
  each triangle's colour and emission sampled from its textures.
- `src/lights.cpp`: the light list ReSTIR DI samples.
- `src/camera.cpp`: the fly camera.
- `src/ui.cpp`: the settings window and the timings it shows.
- `src/settings.h`: the settings the window edits and the renderer reads.
- `src/share.cpp`: the settings and view as a share string (`--settings`,
  the web build's address).
- `src/bookmarks.cpp`, `src/capture.cpp`: camera bookmarks, and PNG
  screenshots through bgfx's callback.
- `web/`: the web build's page (model loading, the address) and a WebGPU
  workaround.
- `.github/`: the native and web CI workflows, and the frame check the
  Linux job runs.
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
