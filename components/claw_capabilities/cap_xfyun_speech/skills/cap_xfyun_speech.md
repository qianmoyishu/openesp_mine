# XFYun Speech

Use this skill when the user asks for iFlytek/XFYun ASR or TTS.

Tools:

- `xfyun_asr`: transcribe a local PCM/WAV audio file.
- `xfyun_tts`: synthesize speech to a local file.

For WeChat/file delivery, prefer WAV output:

```json
{"text":"hello","path":"/fatfs/tts_hello.wav","format":"wav","voice":"xiaoyan"}
```

Use raw/PCM output only when another tool explicitly needs headerless 16 kHz PCM. Do not invent XFYun credentials.
