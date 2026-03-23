# Sunshine / Moonlight VMAF Comparison Tool

A usage guide for `vmaf_compare.py`, a script that aligns Sunshine-side recordings (Reference) with Moonlight-side recordings (Distorted) and computes VMAF and SSIM.

## Prerequisites (Building from GitHub Sources)
- `vmaf` (clone and build from https://github.com/Netflix/vmaf)
  - Example:
    ```bash
    git clone https://github.com/Netflix/vmaf.git
    cd vmaf/libvmaf
    meson setup build --buildtype release
    ninja -C build
    sudo ninja -C build install
    ```
  - Verify with `pkg-config --modversion libvmaf`.
- `ffmpeg` (clone from https://github.com/FFmpeg/FFmpeg and build with libvmaf enabled)
  - Example:
    ```bash
    git clone https://github.com/FFmpeg/FFmpeg.git
    cd FFmpeg
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \  # adjust to where libvmaf was installed
    ./configure --enable-libvmaf --enable-gpl --enable-libx264
    make -j$(nproc)
    sudo make install
    ```
  - Verify with `ffmpeg -filters | grep vmaf` — `libvmaf` should appear.
  - To prioritize a local build, add it to your PATH:
    `export PATH=$HOME/ffmpeg:$PATH` (example when the ffmpeg binary is in `$HOME/ffmpeg`)
- Sunshine-side `.meta` / `.frames.csv` (automatically used if present)
- Moonlight-side recording file and, optionally, its `.frames.csv`

## Minimal Command
Compare without CSV, assuming frame counts match between both sides.
```bash
python3 vmaf_compare.py \
  --reference sunshine_capture.yuv \
  --distorted moonlight_recording.yuv \
  --meta sunshine_capture.yuv.meta
```

## Precise Comparison with Frame Alignment (CSV)
Use `.frames.csv` from both Sunshine and Moonlight to match frames by `frame_nr`, excluding dropped frames.
```bash
python3 vmaf_compare.py \
  --reference sunshine_capture.yuv \
  --distorted moonlight_recording.yuv \
  --ref-csv sunshine_capture.frames.csv \
  --dist-csv moonlight_recording.frames.csv \
  --meta sunshine_capture.yuv.meta
```
The output will also display the drop rate.

## Automatic Parameter Extraction from .meta
When a Sunshine `.meta` file is available, resolution, fps, and source_pix_fmt are read automatically.
```bash
python3 vmaf_compare.py \
  --reference sunshine_capture.yuv \
  --distorted moonlight_recording.yuv \
  --ref-csv sunshine_capture.frames.csv \
  --dist-csv moonlight_recording.frames.csv \
  --meta sunshine_capture.yuv.meta
```
If `--meta` is omitted, the script will automatically look for `<extension>.meta` at the same path as the reference file.

## Comparing Raw Files
Resolution and fps must be specified. Pixel format is passed via `--ref-pixfmt` / `--dist-pixfmt` (e.g., `nv12`, `yuv420p`, `p010`).
```bash
python3 vmaf_compare.py \
  --reference sunshine_capture.yuv --ref-pixfmt yuv420p \
  --distorted moonlight_recording.yuv --dist-pixfmt yuv420p \
  --ref-csv sunshine_capture.frames.csv \
  --dist-csv moonlight_recording.frames.csv \
  --meta sunshine_capture.yuv.meta
```
For container inputs (mkv/mp4), pixel format specification is not required — they are internally decoded to YUV420P for computation.

### Behavior When Pixel Format Is Omitted
- Container input (.mkv/.mp4): Automatically decoded and converted to YUV420P before VMAF calculation.
- Raw input with `.yuv` extension: Defaults to `yuv420p` if not specified.
- Raw input with a non-`.yuv` extension: Exits with an error if `--ref-pixfmt` / `--dist-pixfmt` is omitted.
- If `--meta` is specified and contains `source_pix_fmt`, that value is used automatically even when `--ref-pixfmt` is omitted.

## Measuring Only a Subset of Frames
To compute only the first `N` frames, use `--frames`.
```bash
python3 vmaf_compare.py ... --frames 300
```

## Output Options
- `--output results.json` : Save VMAF/SSIM and per-frame results as JSON
- `--log-file log.txt` : Append key metrics to a log file (`--log-per-frame` to include per-frame data)
- `--verbose` : Display the ffmpeg commands being executed

## Specifying the VMAF Model
- Default: ffmpeg default (equivalent to vmaf_v0.6.1)
- Custom file: `--model-path /path/to/vmaf_v0.6.1.json`

## Recommended Workflow
1. Capture frames on the Sunshine side (`ffv1` or `raw` recommended) while simultaneously recording on the Moonlight side.
2. Gather `.frames.csv` from both sides (Sunshine generates it automatically; obtain the Moonlight one separately).
3. Specify `--meta` to auto-detect resolution and fps.
4. Run the comparison with `--ref-csv` / `--dist-csv` and check the drop rate.
5. Optionally use `--frames` for shorter computation, and `--output` or `--log-file` to save results.

## Error Message Hints
- `ffmpeg does not have libvmaf` → When building ffmpeg from source, include `--enable-libvmaf` and ensure `PKG_CONFIG_PATH` points to libvmaf's pkgconfig directory. Verify with `ffmpeg -filters | grep vmaf`.
- `raw files require --width, --height, --fps` → These are mandatory for raw inputs.
- `CSV file not found` → Double-check the path. If unavailable, run in CSV-less mode (requires matching frame counts).

## What the Output Contains (Display/Saved)
- VMAF mean, min, max, and percentiles (5th, 25th)
- SSIM mean (when computation succeeds)
- Number of frames used and drop rate (in CSV mode)
- Per-frame VMAF/SSIM with corresponding `frame_nr` (in JSON or when `--log-per-frame` is specified)

## Notes
- VMAF calculation is always performed in YUV420P (converted via `format=yuv420p` before libvmaf).
- Temporary files created during frame extraction are automatically deleted.
- Computation uses CPU multi-threading (`n_threads=os.cpu_count()`).
