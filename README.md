# VMAF
VMAF filter for VapourSynth, based on https://github.com/Netflix/vmaf.


## Usage
    vmaf.VMAF(clip reference, clip distorted, string log_path[, int log_format=0, int[] model=[0], int[] feature=[]])

- reference, distorted: Clips to calculate VMAF score. Only YUV format with integer sample type of 8-16 bit depth and chroma subsampling of 420/422/444 is supported.

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

- feature: Additional metrics to enable.
  - 0 = PSNR
  - 1 = PSNR-HVS
  - 2 = SSIM
  - 3 = MS-SSIM
  - 4 = CIEDE2000


## Compilation
Requires `libvmaf`.

```
meson build
ninja -C build install
```
