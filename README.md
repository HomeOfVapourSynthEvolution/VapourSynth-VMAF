# VMAF
Video Multi-Method Assessment Fusion, based on https://github.com/Netflix/vmaf.

Additionally, [vsvmafxml](https://pypi.org/project/vsvmafxml) can be used to store per-frame score from XML log as a frame property in clip.


## Usage
    vmaf.VMAF(vnode reference, vnode distorted, string log_path[, int log_format=0, int[] model=None, int[] feature=None])

- reference, distorted: Clips to compute VMAF score. Only YUV format with integer sample type of 8-16 bit depth and chroma subsampling of 420/422/444 is supported.

- log_path: Path to the log file.

- log_format: Format of the log file.
  - 0 = XML
  - 1 = JSON
  - 2 = CSV
  - 3 = subtitle

- model: Model to use. Refer to [this](https://github.com/Netflix/vmaf/blob/master/resource/doc/models.md), [this](https://netflixtechblog.com/toward-a-better-quality-metric-for-the-video-community-7ed94e752a30) and [this](https://github.com/Netflix/vmaf/blob/master/resource/doc/conf_interval.md) page for more details.
  - 0 = vmaf_v0.6.1 (default mode)
  - 1 = vmaf_v0.6.1neg (NEG mode)
  - 2 = vmaf_b_v0.6.3 (Confidence Interval)
  - 3 = vmaf_4k_v0.6.1

- feature: Additional metrics to compute.
  - 0 = PSNR
  - 1 = PSNR-HVS
  - 2 = SSIM
  - 3 = MS-SSIM
  - 4 = CIEDE2000


---
    vmaf.CAMBI(vnode clip, string log_path[, int log_format=0, int window_size=None, float topk=None, float tvi_threshold=None, int max_log_contrast=None, int enc_width=None, int enc_height=None])

CAMBI (Contrast Aware Multiscale Banding Index) is Netflix's detector for banding (aka contouring) artifacts. For an introduction to CAMBI, please refer to the [tech blog](https://netflixtechblog.medium.com/cambi-a-banding-artifact-detector-96777ae12fe2).

The CAMBI score starts at 0, meaning no banding is detected. A higher CAMBI score means more visible banding artifacts are identified. The maximum CAMBI observed in a sequence is 24 (unwatchable). As a rule of thumb, a CAMBI score around 5 is where banding starts to become slightly annoying (also note that banding is highly dependent on the viewing environment - the brigher the display, and the dimmer the ambient light, the more visible banding is).

- clip: Clip to compute CAMBI score. Only YUV format with integer sample type of 8-16 bit depth and chroma subsampling of 420/422/444 is supported.

- log_path: Path to the log file.

- log_format: Format of the log file.
  - 0 = XML
  - 1 = JSON
  - 2 = CSV
  - 3 = subtitle

- window_size: (min: 15, max: 127, default: 63): Window size to compute CAMBI. (default: 63 corresponds to ~1 degree at 4K resolution and 1.5H)

- topk: (min: 0.0001, max: 1.0, default: 0.6): Ratio of pixels for the spatial pooling computation.

- tvi_threshold: (min: 0.0001, max: 1.0, default: 0.019): Visibilty threshold for luminance ΔL < tvi_threshold*L_mean for BT.1886.

- max_log_contrast: (min: 0, max: 5, default: 2): Maximum contrast in log luma level (2^max_log_contrast) at 10-bits. Default 2 is equivalent to 4 luma levels at 10-bit and 1 luma level at 8-bit. The default is recommended for banding artifacts coming from video compression.

- enc_width, enc_height: Encoding/processing resolution to compute the banding score, useful in cases where scaling was applied to the input prior to the computation of metrics.


## Compilation
Requires `libvmaf`.

```
meson build
ninja -C build
ninja -C build install
```
