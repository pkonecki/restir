# ReSTIR GI

A C++ ReSTIR global illumination example using bgfx. It loads a glTF scene
using fastgltf (by default the bundled open-front Cornell box), then renders it
with a deferred PBR pipeline and a screen-space ReSTIR GI pass.

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

To load another model, pass its path:

```sh
restir_gi --model path/to/scene.gltf
```

The camera orbits the centre of the scene's bounds, and distances in the GI
pass scale with the scene's size. A point light is placed in front of the
scene's emissive surfaces, or near the top of the scene if it has none. Its
intensity scales with the square of the scene's size. Materials use their
base colour, metallic and roughness factors; textures and vertex colours are
not loaded yet, and primitives that are not triangle lists are skipped.

Drag with the left mouse button to orbit, use the mouse wheel to zoom, and
press B to switch between single-bounce and multi-bounce indirect light and S
to turn shadows on or off, and D to turn the denoiser on or off. Surfaces seen from behind are not drawn, so the walls nearest
the camera disappear and the interior stays visible from any side.

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

and open <http://localhost:8080/restir_gi.html>. The web build always loads
the bundled Cornell box, since `--model` has no command line to come from.

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

The GI pass (`shaders/fs_restir_gi.sc`) runs at half resolution. Per pixel it:

1. traces two cosine-distributed rays through the depth buffer and resamples
   the hits into a reservoir, together with the pixel's own reprojected
   reservoir from the previous frame (capped at M = 20);
2. combines that reservoir with the previous-frame reservoirs of four random
   neighbours on the same surface using pairwise MIS, applying the
   reconnection Jacobian. The MIS weights check whether each neighbour could
   have found this pixel's sample, with one screen-space visibility ray per
   neighbour, so occluders next to a pixel do not darken it;
3. tests the chosen sample for visibility from this pixel, shades it, and
   hands the result to the denoiser (below).

Only the reservoir from step 1 is stored as history, along with the
receiver's position and normal, which the MIS weights need.

With multiple bounces enabled, a sample point is shaded with its direct light
plus the indirect light that left it in the previous frame, read back from
the denoised history. Each frame thus adds one bounce, and the lighting
converges to all bounces within a few frames. The composite
pass upsamples the result with a geometry-aware filter and adds direct PBR
lighting.

Known limits: rays only see the depth buffer, so off-screen, occluded and
back-facing geometry cannot contribute, which also caps the extra bounces;
bounced light lags a few frames behind camera motion; shadow edges have a
fixed softness rather than one that matches the size of the light; and
screen-space visibility and the clamped reconnection Jacobian leave a bias of
a few percent. This is a real-time
demonstration, not an unbiased multi-bounce path tracer.

The Cornell box model is "Cornell Box- Original" by t-ly, downloaded from
[Sketchfab](https://sketchfab.com/3d-models/cornell-box-original-0d18de8d108c4c9cab1a4405698cc6b6)
and licensed under CC-BY-4.0. The full attribution and license details are in
[`assets/cornell_box/license.txt`](assets/cornell_box/license.txt).

This work is based on "Cornell Box- Original"
(https://sketchfab.com/3d-models/cornell-box-original-0d18de8d108c4c9cab1a4405698cc6b6)
by t-ly (https://sketchfab.com/t-ly) licensed under CC-BY-4.0
(http://creativecommons.org/licenses/by/4.0/).

## Visual Studio Code

Install the Microsoft C/C++ extension. Use **Ctrl+Shift+B** to configure and
build the Debug target, or select **Debug ReSTIR GI** in the Run and Debug
view (F5) to build and launch it with the shader directory as its working
directory.
