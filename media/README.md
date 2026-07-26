# Display media

`source/standby.mp4` and `source/sedentary-reminder.mp4` are the silent,
renamed source clips supplied for the show-floor build.

The T5 firmware does not include an MP4 decoder. Generate the embedded standby
asset with FFmpeg and the checked-in converter:

```powershell
ffmpeg -i media/source/standby.mp4 -filter_complex `
  "[0:v]fps=8,scale=480:320:force_original_aspect_ratio=increase:flags=lanczos,crop=480:320,split[s0][s1];[s0]palettegen=max_colors=64:stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle" `
  -an media/standby.gif
python tools/gif_to_lvgl_c.py media/standby.gif `
  src/nightshift_idle_gif.c --symbol nightshift_idle_gif
```

`-an` intentionally strips audio. The sedentary clip remains source-only
because the current OPI application has no sedentary-reminder trigger. Do not
extend the frozen T5-Link v1 protocol just for this optional show feature.
